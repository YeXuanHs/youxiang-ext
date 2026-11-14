# 优享财务系统开心扩展

> `youxiang` —— 优享财务系统的内置授权 PHP 扩展。

**版本 1.1.0** · PHP 7.4 ~ 8.3 · MIT · 适配 1.6.2 及以后（含 1.6.4 双文件授权模块）

## 这是什么

**优享财务系统开心扩展**把「跳过授权校验 / 始终识别为企业版」这件事放进 PHP 扩展层：
装好它，优享财务系统的**安装向导与授权判定**按「内置授权」口径运行——
不再要求先购买授权包，授权码仍需填写但内容不限，站点恒识别为**企业版正版授权**。

程序源码不需要任何改动，磁盘上的文件保持原样；卸载只需注掉 `php.ini` 一行。

### 关于命名

- 仓库名：`youxiang-ext`
- 扩展名：`youxiang`（Linux `.so` / Windows `.dll`）
- 正式名称：**优享财务系统开心扩展**

---

## 效果对照

| 场景 | 未装扩展 | 装上 `youxiang.so` |
|---|---|---|
| 安装向导第 2 步「授权前提」 | 提示必须购买授权包，阻断「下一步」 | 显示「已授权 · 无需授权码」，**不再阻断** |
| 安装向导第 3 步 | 出现「未购买授权包 · 本程序不可安装」橙卡 | **该卡片不再出现** |
| 安装向导第 4 步授权码 | 必填，且格式与签名都要对得上 | **仍需填写**，但填什么都通过（任意 md5、任意字符串） |
| 安装判定 | 校验不通过即中止安装 | 恒为通过，版本为企业版 |
| 前台授权角标 | 「域名未在官方授权」 | 「**域名已正版授权**」 |
| 后台授权中心 | 未登记 / 未授权 | 已授权 · 企业版 |
| 版本能力上限 | 受 `license_enforce` 收敛 | **企业版**（普惠 / 专业 / 企业可自由切换） |

## 什么时候用

- 自己部署使用，或给客户做私有部署，不需要走「购买授权包」流程；
- 内部测试 / 演示环境，希望跳过授权环节直接装。

## 环境要求

PHP **7.4 ~ 8.3**（NTS / 非线程安全版）。按下表选对应文件：

| PHP 版本 | 文件 |
|---|---|
| 7.4 | `youxiang-php7.4.so` |
| 8.0 | `youxiang-php8.0.so` |
| 8.1 | `youxiang-php8.1.so` |
| 8.2 | `youxiang-php8.2.so` |
| 8.3 | `youxiang-php8.3.so` |

> Windows 版（`.dll`）暂未提供；如需 Windows 环境，请用仓库里的 `src/youxiang.c` + `config.m4` 自行编译。

## 安装

**1. 下载**：从本仓库 Releases 下载对应版本的 `.so`。

**2. 放到 PHP 扩展目录**：

```bash
php -i | grep '^extension_dir'
# 例：cp youxiang-php8.2.so /www/server/php/82/lib/php/extensions/no-debug-non-zts-20220829/
```

**3. 在 `php.ini` 追加一行**：

```ini
extension=youxiang.so
```

> 宝塔面板：软件商店 → 对应 PHP 版本 → **配置文件** → 追加后**重启 PHP**。

**4. 验证**：

```bash
php -m | grep youxiang
php -r 'print_r(youxiang_status());'
```

输出里有 `youxiang` 即加载成功。

## 确认是否生效

装好扩展后打开安装向导，第 2 步应显示「已授权 · 无需授权码」；安装完成后，前台授权角标应显示「域名已正版授权」。

也可以在服务器上直接查扩展改写了哪些文件：

```bash
php -r 'print_r(youxiang_patched_log());'
```

输出形如：

```
Array
(
    [/var/www/你的站点/includes/license.php] => 1
)
```

1.6.4 起授权模块拆成了 `includes/license/community.php`（开源版）与 `includes/license/official.php`（正版），
此时会显示成：

```
Array
(
    [/var/www/你的站点/includes/license/community.php] => 3
)
```

数字是**命中的改写点数量**：1.6.4 是 11（`query` / `badge` / `isAuthorized` / `isCommunity` / `mode` /
`modeLocked` / `remoteVerifyPackageCode` / `verifyRequest` / `serveQuery` / `servePkgVerify` /
`servePkgSigVerify`），1.6.2 及以前是 1（只有 `query`）。

**输出为空**说明目标文件没有被命中（通常是站点目录不是 `install/install.php` / 授权模块文件，或函数签名与预期不同）——此时扩展不生效，但也不会影响站点正常运行。

### 主动体检（推荐）

`youxiang_patched_log()` 只能看到「运行时实际编译过什么」；想在**部署后立刻**确认锚点是否还对得上，用：

```bash
php -r 'print_r(youxiang_selftest("/var/www/你的站点/includes/license/community.php"));'
```

返回里 `anchors_hit` / `anchors_total` 分别是命中数与总数（1.6.4 应为 11/11），`anchors` 会逐个列出每条签名是否命中，`ok` 为总判定。

### 锚点漂移会告警

如果程序升级后函数签名变了，扩展会匹配到目标文件但一个锚点都插不进去——这是最危险的情况（装上了但界面照旧）。此时扩展会写一条 `E_WARNING` 到 PHP 错误日志（**每个进程只写一次**）：

```
youxiang: 目标文件已匹配，但所有锚点均未命中（/var/www/.../community.php）。
通常是程序更新导致函数签名变化，本次改写无效；请用 youxiang_selftest() 核对锚点。
```

看到这条就该核对签名了，别以为「没报错就是好的」。

**0 命中时行为完全透明**：扩展会释放资源并直接调用原始 `compile_file`，与没装扩展时一模一样（连 opcache 行为都一致），不会用自己的编译路径再跑一遍原逻辑。

## 卸载 / 停用

把 `php.ini` 里的 `extension=youxiang.so` 注释掉，重启 PHP 即可。扩展不写任何文件、不建任何表，卸载后不影响已装站点的数据。

## 注意事项

- 扩展在 `php.ini` 里是**全局生效**的，等于该服务器上所有 PHP 站点都不再校验授权。多站环境建议改用站点目录下的 `user_ini`（`.user.ini`）单独加载，或只在需要的站点启用。
- 扩展只改写「运行期行为」，**不改动磁盘上的任何文件**。
- **opcache 交互**：opcache 作为 `zend_extension` 先于本扩展初始化，因此本扩展的钩子处在最外层。命中目标文件时直接走 `zend_compile_string` 编译，**不经过 opcache**——改写照常生效，但这两个文件失去字节码缓存与 JIT，每次请求重新编译（license 模块约 8KB，毫秒级）。未命中的文件仍走原始 `compile_file`，opcache 照常工作。用 `php -r 'print_r(youxiang_status());'` 可看 `opcache_present` 字段。
- 扩展**不含任何调试输出**，正常运行时不写日志（只有锚点全部未命中时写一条 `E_WARNING`）。

## 原理

扩展接管 PHP 的文件编译入口（`zend_compile_file`）：在编译目标文件时，向目标函数的函数体开头插入一条提前返回的语句，使原函数体变为不可达代码。**磁盘上的源码内容完全不变**，行为差异只存在于内存中的编译结果。

生效的位置：

| 文件 | 函数 | 作用 |
|---|---|---|
| `install/install.php` | `install_check_license()` | 授权码校验恒通过 |
| `install/install.php` | `install_license_required()` | 恒为 false（不再强制购买授权包） |
| `install/install.php` | `install_license_gate_item()` | 第 2 步准入项显示「已授权」 |
| `includes/license.php`（1.6.2 及以前） | `YxLicense::query()` | 恒返回 authorized / enterprise |
| `includes/license/community.php`（1.6.4 开源版） | `query()` / `badge()` / `isAuthorized()` | 恒 authorized / enterprise / 「域名已正版授权」/ true |
| `includes/license/official.php`（1.6.4 正版） | 同上 | 同上 |
| 同上 | `isCommunity()` | false（后台「重新复核」不再被拦，提示为已正版授权） |
| 同上 | `mode()` | 'official'（去掉「当前为开源版」横幅） |
| 同上 | `modeLocked()` | false（解除形态锁定与版本档位锁定） |
| 同上 | `remoteVerifyPackageCode()` | 恒 ok + enterprise（正版包也能离线安装） |
| 同上 | `verifyRequest()` | 放行任意下游请求（不校验签名） |
| 同上 | `serveQuery()` | 恒 authorized / enterprise |
| 同上 | `servePkgVerify()` | 恒 ok + enterprise（安装授权码复核） |
| 同上 | `servePkgSigVerify()` | 恒 ok + enterprise（签名授权文件复核） |

匹配按函数签名字面量进行，未命中即跳过该处（保持原有行为，不会导致报错）。

### 已知局限

| 局限 | 说明 |
|---|---|
| 只拦文件，不拦 `eval` | 钩的是 `zend_compile_file`；`eval()` / `create_function()` 走 `zend_compile_string`，不在覆盖范围内。实际影响很小：授权模块不会以字符串形式被 eval |
| 依赖函数签名 | 官方改动签名（哪怕只是默认值或多一个空格）就会 miss。已有 `E_WARNING` 告警 + `youxiang_selftest()` 兜底 |
| opcache 优化缺席 | 见上文「opcache 交互」：目标文件不走 opcache，失去优化与 JIT |
| 目标文件每次重新编译 | 属上一条的直接后果，文件很小，实测可忽略 |
| 锚点出现在注释里（极低概率） | 匹配器只找第一处命中，若签名先出现在文档注释中，从注释内部起扫会判错位置。当前锚点后无此情况；根治需整文件状态机扫描 |
| ZTS 构建 | 已补 `ZEND_TSRMLS_CACHE_DEFINE()`，ZTS + 动态编译可链接；CI 为 NTS 环境，另有静态宏配对检查兜底 |
| `include 'license.php'` 走 include_path | `expand_filepath()` 按进程 cwd 解析相对路径；若靠 include_path 找到目标文件，算出的 `opened_path` 可能与 PHP 内部的 resolved_path 不一致。目标文件通常是 `require_once __DIR__ . '/...'` 绝对路径，触发不了 |
| 32 位系统大文件 | `yx_read_file` 的 `ftell()` 返回 `long`，>2GB 会溢出。目标文件 8KB，不涉及 |

### 为什么 1.6.4 需要额外改写 `badge()` 与 `isAuthorized()`

1.6.4 起授权模块拆成「开源版 / 正版」两套互不复用的实现，其中开源版把界面标识**硬编码**了：

- `badge()` 直接返回「开源版 · 未启用授权模块」，**不看** `query()` 的返回值；
- `isAuthorized()` 恒返回 `false`。

所以只改写 `query()` 的话，授权结论虽然已是 authorized，**界面角标仍然显示「开源版」**。这两个方法一起改写，前台、后台、授权中心才会统一显示「域名已正版授权」。

## 自行编译

```bash
# 需要 php-dev 工具链：apt install build-essential autoconf pkg-config
phpize
./configure --enable-youxiang
make -j4
# 产物：modules/youxiang.so
```

## 许可证

[MIT](LICENSE) · Copyright (c) 2026 AnchorFlow团队-Sora
