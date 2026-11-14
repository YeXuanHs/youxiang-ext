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
 *     2) function install_license_required(): bool → 恒 false
 *     3) function install_license_gate_item(): array → 恒「已授权 · 无需授权码」
 *   includes/license.php            （1.6.2 及以前：单文件实现）
 *   includes/license/community.php  （1.6.4 起：开源版实现）
 *   includes/license/official.php   （1.6.4 起：正版实现）
 *     4) query()               → authorized + enterprise
 *     5) badge()               → 「域名已正版授权」
 *     6) isAuthorized()        → true
 *     7) isCommunity()         → false（解除「重新复核」拦截）
 *     8) mode()                → 'official'（去掉「当前为开源版」横幅）
 *     9) modeLocked()          → false（解除形态锁定与版本档位锁定）
 *    10) remoteVerifyPackageCode() → ok + enterprise（正版包可离线安装）
 *    11) verifyRequest()       → 放行任意请求
 *    12) serveQuery()          → authorized / enterprise
 *    13) servePkgVerify()      → ok + enterprise
 *    14) servePkgSigVerify()   → ok + enterprise
 *
 * ------------------------------------------------------------------
 * 设计约束（踩过的坑，改动前先读）：
 *
 * 【A】插入体必须完全自包含
 *   只允许用 PHP 内置函数（strtolower / trim / date）与字面量。
 *   不要引用 self::STATUS_OK、self::OFFICIAL_NAME、normalize_domain()、now()
 *   等目标文件里的符号 —— 版本一变方法/常量就没了，引用即 fatal。
 *   唯一允许的同类调用是 badge() 里的 self::query()（两者必同时存在）。
 *   也不要引用 self::$memo：1.6.4 的 community.php 没有该静态属性。
 *
 * 【B】定位 '{' 不能靠 memchr
 *   签名里的默认值可能含 '{'（如 string $code = '{'），必须按括号深度扫描，
 *   并跳过字符串字面量与注释。见 yx_find_body_brace()。
 *
 * 【C】必须主动登记 EG(included_files)
 *   zend_op_array 在 7.4 ~ 8.3 都没有 opened_path 成员，所以不能靠它去重。
 *   真实的去重键是 zend_file_handle.opened_path，但**登记时机随环境变化**：
 *     · 无 opcache：登记发生在 zend_compile_file 之后；我们返回自己的
 *       op_array 后那段代码不再执行 → 文件没被登记 → require_once
 *       认为未包含过 → 二次编译 → "Cannot redeclare class"。
 *     · 有 opcache：opcache 钩子内部会补登记，看起来正常。
 *   因此返回 op_array 前必须自己补一刀：
 *     zend_hash_add_empty_element(&EG(included_files), fh->opened_path)
 *   （幂等：已存在时返回 false，无副作用）
 *
 * 【D】opcache 交互
 *   opcache 是 zend_extension，MINIT 早于本模块，因此 yx_orig_compile_file
 *   指向的是 opcache 的钩子。命中文件时我们直接 zend_compile_string，
 *   不经过 opcache —— 改写**有效**，但目标文件失去字节码缓存与 JIT，
 *   每个请求重新编译。目标文件都很小（license 模块 ~8KB），可接受。
 *   未命中文件仍走 yx_orig_compile_file，opcache 照常工作。
 *
 * 【E】未命中必须可见
 *   锚点漂移是静默失效：官方改一个空格就 miss，代码放行原函数。
 *   因此 hits==0 时写一条 E_WARNING（进程内只警告一次，避免刷屏），
 *   并提供 youxiang_selftest() 主动体检。
 *
 * 【F】ZTS 下日志数组不是线程安全的
 *   yx_log_* 是进程级静态数组，多线程 SAPI 会竞态。用 TSRM 互斥量保护。
 *
 * 编译（Linux / 宝塔）：
 *   phpize && ./configure --enable-youxiang && make -j4
 *   产物：modules/youxiang.so
 * 安装：
 *   extension=youxiang.so
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

#ifdef ZTS
#include "TSRM.h"
#endif

#define YX_EXT_VERSION "1.1.0"

static zend_op_array *(*yx_orig_compile_file)(zend_file_handle *fh, int type);

/* ------------------------------------------------------------------ */
/* 线程安全                                                            */
/* ------------------------------------------------------------------ */

#ifdef ZTS
static MUTEX_T yx_mutex = NULL;
# define YX_LOCK()   do { tsrm_mutex_lock(yx_mutex); } while (0)
# define YX_UNLOCK() do { tsrm_mutex_unlock(yx_mutex); } while (0)
#else
# define YX_LOCK()   do { } while (0)
# define YX_UNLOCK() do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* 诊断日志（进程级；每个文件只记一条）                                */
/* ------------------------------------------------------------------ */

#define YX_LOG_MAX 32
static char yx_log_file[YX_LOG_MAX][512];
static int  yx_log_hits[YX_LOG_MAX];
static int  yx_log_count = 0;

/* 未命中告警：进程内只打一次，避免每请求刷屏 */
static int yx_warned_miss = 0;

static void yx_log(const char *file, int hits)
{
	int i;

	YX_LOCK();
	for (i = 0; i < yx_log_count; i++) {
		if (strcmp(yx_log_file[i], file) == 0) {
			/* 同名文件只保留最大命中数（不同路径可能同名） */
			if (hits > yx_log_hits[i]) {
				yx_log_hits[i] = hits;
			}
			YX_UNLOCK();
			return;
		}
	}
	if (yx_log_count < YX_LOG_MAX) {
		snprintf(yx_log_file[yx_log_count], sizeof(yx_log_file[0]), "%s", file);
		yx_log_hits[yx_log_count] = hits;
		yx_log_count++;
	}
	YX_UNLOCK();
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
		/* 只用内置函数，不引用 normalize_domain() / self::STATUS_OK / now() */
		"\n        return array("
		"'domain' => strtolower(trim((string)$domain)), "
		"'authorized' => true, "
		"'status' => 'authorized', "
		"'edition' => 'enterprise', "
		"'issued_at' => null, "
		"'expire_at' => null, "
		"'license_key' => null, "
		"'official' => 'AnchorFlow团队', "
		"'source' => 'builtin', "
		"'message' => '已正版授权（内置授权包 · 永久有效）', "
		"'checked_at' => date('Y-m-d H:i:s'));"
	},
	{
		/* self::query() 是同类方法，且本文件必然含它（下面也改写），可安全引用 */
		"public static function badge(?string $domain = null): array",
		"\n        return array("
		"'text' => '域名已正版授权', "
		"'class' => 'ok', "
		"'icon' => '\xe2\x9c\x93', "
		"'detail' => self::query($domain));"
	},
	{
		"public static function isAuthorized(?string $domain = null): bool",
		"\n        return true;"
	},
	{
		"public static function isCommunity(): bool",
		"\n        return false;"
	},
	{
		"public static function mode(): string",
		"\n        return 'official';"
	},
	{
		"public static function modeLocked(): bool",
		"\n        return false;"
	},
	{
		"public static function remoteVerifyPackageCode(string $domain, string $product, string $code): ?array",
		"\n        return array("
		"'ok' => true, "
		"'reason' => 'builtin', "
		"'edition' => 'enterprise', "
		"'message' => '已正版授权（内置授权包 · 永久有效）'"
		"); "
	},
	{
		"public static function verifyRequest(array $req): array",
		"\n        return array('ok' => true, 'app' => null, 'msg' => 'ok', 'code' => 200);"
	},
	{
		"public static function serveQuery(?array $app = null): array",
		"\n        return array("
		"'code' => 0, "
		"'msg' => 'ok', "
		"'data' => array("
		"'domain' => strtolower(trim((string)($_REQUEST['domain'] ?? ''))), "
		"'authorized' => true, "
		"'status' => 'authorized', "
		"'official' => 'AnchorFlow团队', "
		"'message' => '已正版授权（内置授权包 · 永久有效）'"
		")); "
	},
	{
		"public static function servePkgVerify(array $req): array",
		"\n        return array("
		"'code' => 0, "
		"'msg' => 'ok', "
		"'data' => array('ok' => true, 'reason' => 'builtin', 'edition' => 'enterprise', "
		"'message' => '已正版授权（内置授权包 · 永久有效）')"
		"); "
	},
	{
		"public static function servePkgSigVerify(array $req): array",
		"\n        return array("
		"'code' => 0, "
		"'msg' => 'ok', "
		"'data' => array('ok' => true, 'reason' => 'builtin', 'edition' => 'enterprise', "
		"'message' => '已正版授权（内置授权包 · 永久有效）')"
		"); "
	},
	{ NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* 源码读取                                                            */
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

/* ------------------------------------------------------------------ */
/* 定位函数体的 '{'                                                    */
/* ------------------------------------------------------------------ */

/*
 * 从 pos 开始向后扫描，返回「括号深度为 0 的第一个 '{'」。
 * 跳过：字符串字面量（单/双引号，含转义）、行注释（// 与 #）、块注释。
 * 这样签名默认值里的 '{'（如 string $x = '{' 或 array('a' => '{')）不会被误判。
 */
static const char *yx_find_body_brace(const char *pos, const char *end)
{
	const char *p = pos;
	int depth = 0;
	char quote = 0;

	while (p < end) {
		char c = *p;

		if (quote) {				/* 字符串内部 */
			if (c == '\\' && (p + 1) < end) { p += 2; continue; }
			if (c == quote) { quote = 0; }
			p++;
			continue;
		}
		if (c == '\'' || c == '"') { quote = c; p++; continue; }
		if (c == '/' && (p + 1) < end && p[1] == '/') {
			while (p < end && *p != '\n') { p++; }
			continue;
		}
		if (c == '#') {
			while (p < end && *p != '\n') { p++; }
			continue;
		}
		if (c == '/' && (p + 1) < end && p[1] == '*') {
			p += 2;
			while ((p + 1) < end && !(p[0] == '*' && p[1] == '/')) { p++; }
			if ((p + 1) < end) { p += 2; }
			continue;
		}
		if (c == '(' || c == '[') { depth++; p++; continue; }
		if (c == ')' || c == ']') { if (depth > 0) { depth--; } p++; continue; }
		if (c == '{') {
			if (depth == 0) {
				return p;
			}
			depth++;
			p++;
			continue;
		}
		if (c == '}') { if (depth > 0) { depth--; } p++; continue; }
		p++;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 源码改写                                                            */
/* ------------------------------------------------------------------ */

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
		/* 从锚点之后开始扫描，避免签名本身被当成函数体 */
		brace = yx_find_body_brace(pos + alen, hay + hlen);
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
		code = tag + 5;
		code_len = (size_t)((ZSTR_VAL(src) + ZSTR_LEN(src)) - code);
	}
	return zend_string_init(code, code_len, 0);
}

/* ------------------------------------------------------------------ */
/* 目标文件判定                                                        */
/* ------------------------------------------------------------------ */

static const yx_patch_t *yx_pick_patches(const char *fn)
{
	if (strstr(fn, "install/install.php") || strstr(fn, "install\\install.php")) {
		return yx_patches_install;
	}
	/* 1.6.2 及以前：单文件 */
	if (strstr(fn, "includes/license.php") || strstr(fn, "includes\\license.php")) {
		return yx_patches_license;
	}
	/* 1.6.4 起：拆成 community / official 两个模块
	   注意这两个路径不含 "includes/license.php" 子串，必须显式列出 */
	if (strstr(fn, "includes/license/community.php") || strstr(fn, "includes\\license\\community.php")
	 || strstr(fn, "includes/license/official.php")  || strstr(fn, "includes\\license\\official.php")) {
		return yx_patches_license;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* 编译钩子                                                            */
/* ------------------------------------------------------------------ */

static zend_op_array *yx_compile_file(zend_file_handle *fh, int type)
{
	const char *fn = NULL;
	const yx_patch_t *patches = NULL;

	/* 注意：include 传给编译器的 type 是 ZEND_INCLUDE / ZEND_REQUIRE，
	 * 不是 ZEND_HANDLE_FILENAME —— 不能按 type 过滤，否则钩子会被整段短路。 */
	/* 路径来源：opened_path（zend_string*，7.4 ~ 8.3 一致）。
	 * filename 兜底，类型随版本变化：
	 *   PHP 7.4 / 8.0  → const char *
	 *   PHP 8.1 ~ 8.3  → zend_string *   */
	if (fh->opened_path) {
		fn = ZSTR_VAL(fh->opened_path);
	}
	if (!fn || !*fn) {
#if PHP_VERSION_ID >= 80100
		fn = fh->filename ? ZSTR_VAL(fh->filename) : NULL;
#else
		fn = fh->filename;
#endif
	}
	if (fn && !*fn) {
		fn = NULL;
	}
	if (fn) {
		patches = yx_pick_patches(fn);
	}

	/* 【关键修复】补上 fh->opened_path
	 * opened_path 是原始 compile_file 内部（open_file_for_scanning）才设置的，
	 * 我们替换了 compile_file 且不调用它 → opened_path 恒为 NULL。
	 * 而 PHP 在 include 处理里用 opened_path 作为 EG(included_files) 的键，
	 * 键为 NULL 时登记失败 → require_once 认为"没包含过" → 二次编译 →
	 * "Cannot redeclare class"。有 opcache 时是 opcache 的钩子顺手设了它，
	 * 所以只有"无 opcache"环境复现。
	 * 这里用绝对路径补上（与 require_once 的 resolved_path 同口径）。 */
	if (!fh->opened_path && fn && *fn) {
		char resolved[MAXPATHLEN];
		if (expand_filepath(fn, resolved)) {
			fh->opened_path = zend_string_init(resolved, strlen(resolved), 0);
		}
	}

	if (patches) {
		zend_string *src = yx_read_file(fn);

		if (src) {
			int hits = 0;
			zend_string *patched = yx_patch_source(src, patches, &hits);
			zend_string *code = yx_strip_open_tag(patched);
			zval zv;
			zend_op_array *op_array;

			yx_log(fn, hits);

			/* 锚点全部未命中 = 签名漂移，此时改写不产生任何效果。
			   这是最危险的情况（装上了但界面照旧），必须留痕。
			   进程内只告警一次，避免每请求刷屏。 */
			if (hits == 0) {
				int need_warn = 0;

				YX_LOCK();
				if (!yx_warned_miss) {
					yx_warned_miss = 1;
					need_warn = 1;
				}
				YX_UNLOCK();

				if (need_warn) {
					zend_error(E_WARNING,
						"youxiang: 目标文件已匹配，但所有锚点均未命中（%s）。"
						"通常是程序更新导致函数签名变化，本次改写无效；"
						"请用 youxiang_selftest() 核对锚点。", fn);
				}
			}

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
			zend_string_release(src);

			if (op_array) {
				/* 【必须主动登记 included_files】
				 * 实测（探针验证）：不同环境下 PHP 把本文件登记进
				 * EG(included_files) 的时机不一致 ——
				 *   · 无 opcache 时，登记发生在 zend_compile_file 之后，
				 *     而我们返回自己的 op_array 后该路径不再执行 → 登记丢失；
				 *   · 有 opcache 时，opcache 的钩子内部会补上，所以看起来正常。
				 * 后果：require_once 认为"没包含过"，二次编译 → Cannot redeclare。
				 * 因此这里以 fh->opened_path 为键补登记一次（幂等，已存在则返回 false）。 */
				if (fh->opened_path) {
					zend_hash_add_empty_element(&EG(included_files), fh->opened_path);
				}
				return op_array;
			}
		}
	}
	return yx_orig_compile_file(fh, type);
}

/* ------------------------------------------------------------------ */
/* 扩展函数                                                            */
/* ------------------------------------------------------------------ */

/**
 * 已改写文件与命中数（进程内累计，同名文件只保留一条）
 */
PHP_FUNCTION(youxiang_patched_log)
{
	int i;

	array_init(return_value);
	YX_LOCK();
	for (i = 0; i < yx_log_count; i++) {
		add_assoc_long(return_value, yx_log_file[i], yx_log_hits[i]);
	}
	YX_UNLOCK();
}

/**
 * 主动体检：对给定文件跑一遍锚点匹配，返回每个锚点是否命中。
 * 与 youxiang_patched_log() 的区别：那个是"运行时实际编译过什么"，
 * 这个是"现在能不能命中"，可以在部署后立刻验证，不必等到触发页面。
 */
PHP_FUNCTION(youxiang_selftest)
{
	zend_string *path = NULL;
	const yx_patch_t *patches;
	zend_string *src;
	int i, hits = 0, total = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	patches = yx_pick_patches(ZSTR_VAL(path));
	if (!patches) {
		array_init(return_value);
		add_assoc_bool(return_value, "target", 0);
		add_assoc_string(return_value, "error", "该路径不是目标文件（install/install.php 或 includes/license*.php）");
		return;
	}

	src = yx_read_file(ZSTR_VAL(path));
	if (!src) {
		array_init(return_value);
		add_assoc_bool(return_value, "target", 1);
		add_assoc_string(return_value, "error", "文件读取失败");
		return;
	}

	array_init(return_value);
	add_assoc_bool(return_value, "target", 1);
	add_assoc_string(return_value, "file", ZSTR_VAL(path));

	{
		zval missing;
		array_init(&missing);
		for (i = 0; patches[i].anchor != NULL; i++) {
			zval ok;
			int found = (zend_memnstr(ZSTR_VAL(src), patches[i].anchor,
					strlen(patches[i].anchor), ZSTR_VAL(src) + ZSTR_LEN(src)) != NULL);
			total++;
			if (found) { hits++; }
			ZVAL_BOOL(&ok, found);
			add_assoc_zval_ex(&missing, patches[i].anchor, strlen(patches[i].anchor), &ok);
		}
		add_assoc_long(return_value, "anchors_total", total);
		add_assoc_long(return_value, "anchors_hit", hits);
		add_assoc_zval(return_value, "anchors", &missing);
	}
	add_assoc_bool(return_value, "ok", (hits == total));

	zend_string_release(src);
}

/**
 * 扩展状态
 */
PHP_FUNCTION(youxiang_status)
{
	int opcache = 0;

	array_init(return_value);
	add_assoc_bool(return_value, "enabled", 1);
	add_assoc_string(return_value, "extension", "youxiang");
	add_assoc_string(return_value, "version", YX_EXT_VERSION);
	add_assoc_string(return_value, "php_version", PHP_VERSION);
	add_assoc_string(return_value, "mechanism", "compile-time source rewrite (zend_compile_file)");

	/* opcache 是否在场：在场时目标文件会绕过其缓存（每次重新编译） */
	opcache = zend_hash_str_exists(&module_registry, "Zend OPcache", sizeof("Zend OPcache") - 1) ? 1 : 0;
	add_assoc_bool(return_value, "opcache_present", opcache);
	add_assoc_string(return_value, "opcache_note", opcache
		? "目标文件（license 模块 / 安装向导）绕过 opcache 重新编译，属预期行为；其余文件仍走 opcache"
		: "未检测到 opcache");

	add_assoc_long(return_value, "anchors_license", 11);
	add_assoc_long(return_value, "anchors_install", 3);
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_youxiang_selftest, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry youxiang_functions[] = {
	PHP_FE(youxiang_status, arginfo_youxiang_status)
	PHP_FE(youxiang_patched_log, arginfo_youxiang_patched_log)
	PHP_FE(youxiang_selftest, arginfo_youxiang_selftest)
	PHP_FE_END
};

PHP_MINIT_FUNCTION(youxiang)
{
#ifdef ZTS
	yx_mutex = tsrm_mutex_alloc();
#endif
	yx_orig_compile_file = zend_compile_file;
	zend_compile_file = yx_compile_file;
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(youxiang)
{
	if (yx_orig_compile_file) {
		zend_compile_file = yx_orig_compile_file;
		yx_orig_compile_file = NULL;
	}
#ifdef ZTS
	if (yx_mutex) {
		tsrm_mutex_free(yx_mutex);
		yx_mutex = NULL;
	}
#endif
	return SUCCESS;
}

PHP_RINIT_FUNCTION(youxiang)
{
#if defined(ZTS) && defined(COMPILE_DL_YOUXIANG)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}

PHP_MINFO_FUNCTION(youxiang)
{
	char buf[64];

	php_info_print_table_start();
	php_info_print_table_row(2, "youxiang support", "enabled");
	php_info_print_table_row(2, "version", YX_EXT_VERSION);
	php_info_print_table_row(2, "mechanism", "compile-time source rewrite");
	php_info_print_table_row(2, "anchors", "license 11 / install 3");
	snprintf(buf, sizeof(buf), "%d",
		zend_hash_str_exists(&module_registry, "Zend OPcache", sizeof("Zend OPcache") - 1) ? 1 : 0);
	php_info_print_table_row(2, "opcache present", buf);
	php_info_print_table_end();
}

zend_module_entry youxiang_module_entry = {
	STANDARD_MODULE_HEADER,
	"youxiang",
	youxiang_functions,
	PHP_MINIT(youxiang),
	PHP_MSHUTDOWN(youxiang),
	PHP_RINIT(youxiang),
	NULL,				/* request_shutdown */
	PHP_MINFO(youxiang),
	YX_EXT_VERSION,
	STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(youxiang)
