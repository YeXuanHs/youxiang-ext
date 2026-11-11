/*
 * youxiang —— 优享财务系统「内置授权」PHP 扩展
 *
 * 目标：把「跳过授权校验 / 始终识别为企业版」的行为放在扩展层，
 *       业务源码在磁盘上**一个字不改**。
 *
 * 实现：接管 zend_compile_file，在编译目标文件时把源码里
 *       「函数体开头 {」之后插入一条 return，原函数体保留但不可达。
 *       —— 为什么不用 zend_execute_ex：PHP 8 的 VM 调用户函数不再经过
 *       zend_execute_ex（只有 PHP 7.x 才走），实测拦截全部失效。
 *
 * 改写点（anchor 为函数/方法签名，未命中即跳过，保证容错）：
 *   install/install.php
 *     1) function install_check_license(...)      → 恒返回 ok + edition=enterprise
 *          第 4 步授权码：必须填，但填什么都过（任意 md5、任意字符串）
 *     2) function install_license_required(): bool → 恒 false
 *          解掉「必须购买授权包才能安装」：
 *            - 第 2 步「授权前提」项不再阻断下一步
 *            - 第 3 步「未购买授权包 · 本程序不可安装」橙卡不再出现
 *            - 第 4 步「未填写授权码」红色警示不再出现
 *     3) function install_license_gate_item(): array → 恒「已授权 · 无需授权码」
 *   includes/license.php
 *     4) public static function query(...)          → 恒返回 authorized + enterprise
 *          前台角标「域名已正版授权」、后台授权中心、版本上限 maxEdition 全部为企业版
 *
 * 编译（Linux / 宝塔）：
 *   phpize && ./configure --enable-youxiang && make -j4
 *   产物：modules/youxiang.so
 * 安装：
 *   extension=youxiang.so   （写进 php.ini）
 *
 * 兼容：PHP 7.4 ~ 8.3
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_compile.h"
#include "zend_execute.h"
#include "zend_operators.h"
#include "zend_API.h"
#include "ext/standard/info.h"

#define YX_EXT_VERSION "1.0.0"

static zend_op_array *(*yx_orig_compile_file)(zend_file_handle *fh, int type);

/* 诊断：记录被改写的文件与命中的锚点数量 */
#define YX_LOG_MAX 32
static char yx_log_file[YX_LOG_MAX][512];
static int  yx_log_hits[YX_LOG_MAX];
static int  yx_log_count = 0;

/* 记录改写情况（始终保留：部署后可用 youxiang_patched_log() 自查锚点是否命中） */
static void yx_log(const char *file, int hits)
{
	if (yx_log_count < YX_LOG_MAX) {
		snprintf(yx_log_file[yx_log_count], sizeof(yx_log_file[0]), "%s", file);
		yx_log_hits[yx_log_count] = hits;
		yx_log_count++;
	}
}

/* ------------------------------------------------------------------ */
/* 改写规则                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *anchor;	/* 函数 / 方法签名 */
	const char *insert;	/* 插入到函数体 '{' 之后的 PHP 代码 */
} yx_patch_t;

static const yx_patch_t yx_patches_install[] = {
	{
		"function install_check_license(string $domain, string $key, bool $required = false): array",
		"\n    return array("
		"'level' => 'ok', "
		"'msg' => '已授权：本程序包内置授权，授权码由发行方签发（企业版 · 永久有效）。', "
		"'edition' => 'enterprise');"
	},
	{
		"function install_license_required(): bool",
		"\n    return false;"
	},
	{
		"function install_license_gate_item(): array",
		"\n    return array("
		"'name' => '安装授权', "
		"'val' => '已授权 · 无需授权码', "
		"'status' => 'ok', "
		"'hint' => '本程序包已内置授权，无需购买授权包即可安装；安装完成后即为正版授权（企业版）。', "
		"'required' => false);"
	},
	{ NULL, NULL }
};

static const yx_patch_t yx_patches_license[] = {
	{
		"public static function query(string $domain, bool $force = false): array",
		"\n        return self::$memo[$domain] = array("
		"'domain' => normalize_domain($domain), "
		"'authorized' => true, "
		"'status' => self::STATUS_OK, "
		"'edition' => 'enterprise', "
		"'issued_at' => null, "
		"'expire_at' => null, "
		"'license_key' => null, "
		"'official' => self::OFFICIAL_NAME, "
		"'source' => 'builtin', "
		"'message' => '已正版授权（内置授权包 · 永久有效）', "
		"'checked_at' => now());"
	},
	{ NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* 源码读取与改写                                                      */
/* ------------------------------------------------------------------ */

static zend_string *yx_read_file(const char *path)
{
	FILE *fp;
	long len;
	char *buf;
	zend_string *s;

	fp = fopen(path, "rb");
	if (!fp) {
		return NULL;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	len = ftell(fp);
	if (len <= 0) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	buf = emalloc((size_t)len + 1);
	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		efree(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	s = zend_string_init(buf, (size_t)len, 0);
	efree(buf);
	return s;
}

/* 在每个命中的 anchor 之后（函数体 '{' 之后）插入 insert；未命中则原样返回 */
static zend_string *yx_patch_source(zend_string *src, const yx_patch_t *patches, int *hits_out)
{
	zend_string *cur = src;
	int i;
	int hits = 0;

	for (i = 0; patches[i].anchor != NULL; i++) {
		const char *a = patches[i].anchor;
		size_t alen = strlen(a);
		const char *hay, *pos, *brace;
		size_t hlen, off, ilen;
		zend_string *ns;

		hay = ZSTR_VAL(cur);
		hlen = ZSTR_LEN(cur);
		pos = zend_memnstr(hay, a, alen, hay + hlen);
		if (!pos) {
			continue;	/* 未命中：容错，保持原样 */
		}
		brace = memchr(pos, '{', (size_t)(hay + hlen - pos));
		if (!brace) {
			continue;
		}
		off = (size_t)(brace - hay) + 1;
		ilen = strlen(patches[i].insert);

		ns = zend_string_alloc(hlen + ilen, 0);
		memcpy(ZSTR_VAL(ns), hay, off);
		memcpy(ZSTR_VAL(ns) + off, patches[i].insert, ilen);
		memcpy(ZSTR_VAL(ns) + off + ilen, hay + off, hlen - off);
		ZSTR_VAL(ns)[ZSTR_LEN(ns)] = '\0';

		if (cur != src) {
			zend_string_release(cur);
		}
		cur = ns;
		hits++;
	}
	if (hits_out) {
		*hits_out = hits;
	}
	return cur;
}


/* zend_compile_string 按「代码片段」编译（类似 eval），不能带 <?php 开头 */
static zend_string *yx_strip_open_tag(zend_string *src)
{
	const char *code = ZSTR_VAL(src);
	size_t code_len = ZSTR_LEN(src);
	const char *tag = strstr(code, "<?php");

	if (tag) {
		code += 5;
		code_len = (size_t)((ZSTR_VAL(src) + ZSTR_LEN(src)) - code);
	}
	return zend_string_init(code, code_len, 0);
}

/* ------------------------------------------------------------------ */
/* 编译钩子                                                            */
/* ------------------------------------------------------------------ */

static zend_op_array *yx_compile_file(zend_file_handle *fh, int type)
{
	/* 文件名可能挂在 filename，也可能只在 opened_path，两个都取 */
	const char *fn = NULL;
	const yx_patch_t *patches = NULL;

	/* 注意：include 传给编译器的 type 是 ZEND_INCLUDE / ZEND_REQUIRE，
	 * 不是 ZEND_HANDLE_FILENAME —— 不能按 type 过滤，否则钩子会被整段短路。 */
	/* PHP 8 的 include 走 zend_stream_init_filename_ex：filename 为空，
	 * 真实路径在 opened_path（PHP 8 是 zend_string*，PHP 7 是 char*）。 */
	/* opened_path 在 PHP 7.4 ~ 8.3 全都是 zend_string*，编译前已填好真实路径 */
	fn = NULL;
	if (fh->opened_path) {
		fn = ZSTR_VAL(fh->opened_path);
	}
	if (!fn || !*fn) {
		fn = fh->filename;	/* 兜底：全版本都是 const char* */
	}
	if (fn && !*fn) {
		fn = NULL;
	}
	if (fn) {
		if (strstr(fn, "install/install.php") || strstr(fn, "install\\install.php")) {
			patches = yx_patches_install;
		} else if (strstr(fn, "includes/license.php") || strstr(fn, "includes\\license.php")) {
			patches = yx_patches_license;
		}
	}
	if (patches) {
		{
			zend_string *src = yx_read_file(fn);
			if (src) {
				int hits = 0;
				zend_string *patched = yx_patch_source(src, patches, &hits);
				zend_string *code = yx_strip_open_tag(patched);
				zval zv;
				zend_op_array *op_array;

				yx_log(fn, hits);
#if PHP_VERSION_ID >= 80200
				/* PHP 8.2+：(zend_string *source, const char *filename, zend_compile_position) */
				op_array = zend_compile_string(code, fn, ZEND_COMPILE_POSITION_AFTER_OPEN_TAG);
#elif PHP_VERSION_ID >= 80000
				/* PHP 8.0~8.1：(zend_string *source, const char *filename) */
				op_array = zend_compile_string(code, fn);
#else
				/* PHP 7.4：(zval *source, char *filename) */
				ZVAL_STR(&zv, code);
				op_array = zend_compile_string(&zv, (char *)fn);
#endif
				zend_string_release(code);
				if (patched != src) {
					zend_string_release(patched);
				}
				if (op_array) {
					zend_string_release(src);
					return op_array;
				}
				zend_string_release(src);
			}
		}
	}
	return yx_orig_compile_file(fh, type);
}

/* ------------------------------------------------------------------ */
/* 扩展函数                                                            */
/* ------------------------------------------------------------------ */

/**
 * 扩展状态自检
 */
PHP_FUNCTION(youxiang_patched_log)
{
	int i;

	array_init(return_value);
	for (i = 0; i < yx_log_count; i++) {
		add_assoc_long(return_value, yx_log_file[i], yx_log_hits[i]);
	}
}

PHP_FUNCTION(youxiang_status)
{
	array_init(return_value);
	add_assoc_bool(return_value, "enabled", 1);
	add_assoc_string(return_value, "extension", "youxiang");
	add_assoc_string(return_value, "version", YX_EXT_VERSION);
	add_assoc_string(return_value, "php_version", PHP_VERSION);
	add_assoc_string(return_value, "mechanism", "compile-time source rewrite (zend_compile_file)");
	add_assoc_string(return_value, "patched",
		"install_check_license, install_license_required, install_license_gate_item, YxLicense::query");
	add_assoc_string(return_value, "notice",
		"授权码仍需填写（任意内容均可通过）；不再要求购买授权包；站点恒识别为企业版授权。");
}

/* ------------------------------------------------------------------ */
/* 模块入口                                                            */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_youxiang_status, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_youxiang_patched_log, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry youxiang_functions[] = {
	PHP_FE(youxiang_status, arginfo_youxiang_status)
	PHP_FE(youxiang_patched_log, arginfo_youxiang_patched_log)
	PHP_FE_END
};

PHP_MINIT_FUNCTION(youxiang)
{
	yx_orig_compile_file = zend_compile_file;
	zend_compile_file = yx_compile_file;
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(youxiang)
{
	if (yx_orig_compile_file) {
		zend_compile_file = yx_orig_compile_file;
	}
	return SUCCESS;
}

PHP_MINFO_FUNCTION(youxiang)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "youxiang support", "enabled");
	php_info_print_table_row(2, "version", YX_EXT_VERSION);
	php_info_print_table_row(2, "mechanism", "compile-time source rewrite");
	php_info_print_table_row(2, "patched",
		"install_check_license, install_license_required, install_license_gate_item, YxLicense::query");
	php_info_print_table_end();
}

zend_module_entry youxiang_module_entry = {
	STANDARD_MODULE_HEADER,
	"youxiang",
	youxiang_functions,
	PHP_MINIT(youxiang),
	PHP_MSHUTDOWN(youxiang),
	NULL,				/* request_startup */
	NULL,				/* request_shutdown */
	PHP_MINFO(youxiang),/* info_func */
	YX_EXT_VERSION,
	STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(youxiang)
