# youxiang

**优享财务系统 · 内置授权扩展** · 版本 1.3.0 · PHP 7.4 ~ 8.3 · MIT

装上这个扩展后，优享财务系统的**安装向导与授权判定**按「内置授权」口径运行：不再要求先购买授权包，授权码仍需填写但内容不限，站点恒识别为**企业版正版授权**。

程序源码不需要任何改动，磁盘上的文件保持原样。

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

数字是**命中的改写点数量**：1.6.4 是 10（`query` / `badge` / `isAuthorized` / `isCommunity` / `mode` /
`modeLocked` / `verifyRequest` / `serveQuery` / `servePkgVerify` / `servePkgSigVerify`），1.6.2 及以前是 1（只有 `query`）。

**输出为空**说明目标文件没有被命中（通常是站点目录不是 `install/install.php` / 授权模块文件，或函数签名与预期不同）——此时扩展不生效，但也不会影响站点正常运行。

## 卸载 / 停用

把 `php.ini` 里的 `extension=youxiang.so` 注释掉，重启 PHP 即可。扩展不写任何文件、不建任何表，卸载后不影响已装站点的数据。

## 注意事项

- 扩展在 `php.ini` 里是**全局生效**的，等于该服务器上所有 PHP 站点都不再校验授权。多站环境建议改用站点目录下的 `user_ini`（`.user.ini`）单独加载，或只在需要的站点启用。
- 扩展只改写「运行期行为」，**不改动磁盘上的任何文件**；但被改写的文件不会进入 opcache 缓存，每次请求会重新编译（约 60KB，开销毫秒级）。介意的话可在该站点设置 `opcache.enable=0`。
- 扩展**不含任何调试输出**，不会往日志里写东西。

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
| 同上 | `verifyRequest()` | 放行任意下游请求（不校验签名） |
| 同上 | `serveQuery()` | 恒 authorized / enterprise |
| 同上 | `servePkgVerify()` | 恒 ok + enterprise（安装授权码复核） |
| 同上 | `servePkgSigVerify()` | 恒 ok + enterprise（签名授权文件复核） |

匹配按函数签名字面量进行，未命中即跳过该处（保持原有行为，不会导致报错）。

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
