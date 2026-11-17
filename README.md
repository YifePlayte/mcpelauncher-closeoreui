# CloseOreUI

关闭 Minecraft 基岩版 **OreUI**、回退到旧 JSON-UI 的模组，唯一构建目标是
[mcpelauncher](https://github.com/minecraft-linux/mcpelauncher-manifest)（Linux x86_64）。

这是一个**独立实现**，不是任何项目的 fork。下文列出参考过的项目、复用的接口，
以及没有复用的部分。

## 它做什么

游戏里有一个「OreUI 配置注册函数」，把每个界面的路由串（`/main-menu`、
`/send-invites` …）连同两个 `std::function<bool()>` 判定器塞进一张 `unordered_map`。
那两个判定器就是界面的开关：`true` 走 OreUI，`false` 回退旧 UI。

本模组在该函数返回后，把每个条目的两个判定器改写为固定值。

那个函数没有导出符号，因此位置**在运行时按结构确定**，而不是靠会随版本失效的字节特征码：

1. 扫描可读段，收集形如路由的字符串常量；
2. 扫描 `.text` 里对这些字符串的 `lea reg, [rip+disp]` 引用；
3. 用 `.eh_frame` 函数表把引用归到所属函数，并确保命中的是**函数入口**；
4. 按**顶层路由数**（`/main-menu` 这类单段路径）排序，最多的即目标。

第 4 步是关键：按「引用路由总数」排序会选错函数。

## 来源与依赖

### 运行时依赖

`mcpelauncher` 是**唯一**构建目标与运行环境。模组是 Android ABI 的 `.so`，
由启动器的 bionic linker 加载，调用 `mod_preinit` / `mod_init`。

| 依赖 | 来源 | 说明 |
|---|---|---|
| `liblog.so` / `libdl.so` | mcpelauncher 内置 linker | 启动器把 `__android_log_*` 与 `dl*` 映射到自身实现 |
| `libc.so` / `libc++_shared.so` | 游戏 APK | 由启动器加载 |
| C++ 标准库 | NDK | 与游戏同一套 libc++ ABI |

**没有其他第三方依赖。** 项目内自行实现了 JSON 解析、x86-64 指令解码、
ELF 与 `.eh_frame` 解析，因此不需要 nlohmann/json、funchook、capstone 等库。

### 参考的项目

| 项目 | 参考了什么 | 是否复用代码 |
|---|---|---|
| [QYCottage/ForceCloseOreUI](https://github.com/QYCottage/ForceCloseOreUI) | `OreUIConfig` 结构体布局（两个指针 + 两个 `std::function<bool()>`），以及「改写判定器」这一机制 | **否**。该项目无 LICENSE 文件（保留所有权利），本项目只采用逆向得出的事实性结构信息，代码为独立实现 |
| [yinghuajimew/closeoreUI-signature](https://github.com/yinghuajimew/closeoreUI-signature) | 「用路由字符串反推函数位置」的思路，以及它记录的几类失败模式（例如特征码命中函数中段） | **否**。其工具针对 AArch64 特征码推导，与本项目的 x86-64 结构定位不共享代码 |
| [mcpelauncher](https://github.com/minecraft-linux/mcpelauncher-manifest) | mod ABI、linker 行为、`mcpelauncher_package_version_*` 等导出符号 | 仅使用公开接口 |

### 与上游的能力差异

上游两个项目有不同的分工：closeoreUI-signature 是**离线工具**，产出一条特征码文本，
需要人工贴进 ForceCloseOreUI 源码再重新编译；ForceCloseOreUI 则维护一张
**累积式特征码表**，历史上每个版本攒一条，新版本出现而没有匹配项时就失效。

| 能力 | closeoreUI-signature | ForceCloseOreUI | 本项目 |
|---|---|---|---|
| 形态 | 离线 Python 工具 | 运行时 mod | 运行时 mod |
| 分析对象 | 磁盘上的 `.so` | 内存中已加载镜像 | 内存中已加载镜像 |
| 输出 | 一条特征码文本 | 直接生效 | 直接生效 |
| 支持新版本 | 跑工具 + 人工贴码 + 重编译 | 等他人推新码 + 重编译 | 启动时自算，无需操作 |
| 函数入口可靠性 | 启发式回溯（有命中的中段的记录） | 特征码匹配 | `.eh_frame` 精确成员测试 |
| 区分候选函数 | 序言形状打分 | 不涉及 | 顶层路由计数 |
| 硬编码路由串 | 是（8 条锚点） | 否 | 否（纯结构判定） |
| 架构 | AArch64 | AArch64 / x86-64 (Windows) | x86-64 (Linux) |

### 贡献归属

| 事项 | 来源 | 说明 |
|---|---|---|
| `OreUIConfig` 结构体布局 | 上游 ForceCloseOreUI | 逆向结论：两个指针 + 两个 `std::function<bool()>` |
| 「改写判定器」的机制 | 上游 ForceCloseOreUI | 本项目只换了「找到函数」的方法 |
| 「用路由串反推函数」的思路 | 上游 closeoreUI-signature | 它用 `ADRP+ADD` 找字符串引用 |
| 特征码会命中函数中段的教训 | 上游 closeoreUI-signature | 其 SIG_MATRIX 记录了实测案例，本项目据此改用 `.eh_frame` |
| 运行时解析 ELF 内存镜像 | 本项目 | 从 `dl_iterate_phdr` 取基址，直接读进程内存 |
| 运行时解析 `.eh_frame_hdr` | 本项目 | 用于把引用归属到函数，并保证命中的是函数入口 |
| x86-64 指令长度解码器 | 本项目 | 用于 `lea` 识别与 hook 序言重定位 |
| 顶层路由判据 | 本项目 | 实测发现按「引用路由总数」排序会选错函数 |
| 内联 hook（rel32 + trampoline） | 本项目 | 含 RIP 相对重定位与函数边界校验 |

两点需要说明清楚：

- **「运行时分析」并非单纯的技术高下之分。** 上游走离线路线有其原因：ARM 设备上跑工具
  更麻烦，而累积的特征码表能覆盖历史版本。x86-64 + mcpelauncher 这个场景天然适合运行时
  分析——启动器本身跑在 Linux 上，能直接访问完整的 ELF 与 `.eh_frame`。这是场景优势。
- **本项目并非全面优于上游。** 上游的特征码表可覆盖 AArch64 多个版本；本项目目前只验证了
  1.26.62.1，且行为层（结构体布局、detour 签名、libc++ ABI）仍依赖上游逆向结论继续成立。

### 许可

**GNU Affero General Public License v3.0**（见 [LICENSE](LICENSE)）。

选择 AGPL 而非 GPL/MIT 的原因与代码来源无关——本项目没有复制 closeoreUI-signature
的代码，其工具针对 AArch64 特征码推导，与这里的 x86-64 结构定位不共享实现。
一致性选在理念上：AGPL 第 13 条要求**通过网络提供服务时也要公开修改后的源码**。
按路由结构定位游戏函数的这套逻辑，如果被包成「上传 .so 返回可用信息」的在线服务，
修改必须回流。

对你的实际影响：

| 场景 | 需要做什么 |
|---|---|
| 自己用、装在本地玩游戏 | 不需要做任何事 |
| 分发修改版（含二进制） | 提供完整对应源码，保留许可与版权声明 |
| 把它做成网络服务 | 同样需要公开服务端修改后的源码 |

仅使用本模组（不修改、不再分发）不触发任何开源义务。

发布包（`.tar.gz`）内含 LICENSE 全文，符合 AGPL 第 6 条对二进制分发的要求。

## 安装

```bash
./scripts/build.sh

# 原生安装
mkdir -p ~/.local/share/mcpelauncher/mods
cp build/x86_64/libcloseoreui.so ~/.local/share/mcpelauncher/mods/

# 或 Flatpak
mkdir -p ~/.var/app/io.mrarm.mcpelauncher/data/mcpelauncher/mods
cp build/x86_64/libcloseoreui.so ~/.var/app/io.mrarm.mcpelauncher/data/mcpelauncher/mods/
```

也可以通过 [release workflow](.github/workflows/release.yml) 构建发布包。

> 启动器的 mod **列表**里不会出现本模组：那个列表只显示启动器自己维护的条目，
> 不扫描第三方 `.so`。这不影响功能，ModLoader 照常加载。

## 配置

首次运行自动在模组目录旁生成 `CloseOreUI/config.json`：

```json
{
    "enabled": true,
    "defaultScreenValue": false,
    "screens": {}
}
```

| 字段 | 作用 |
|---|---|
| `enabled` | 总开关。`false` 时加载但不碰游戏 |
| `defaultScreenValue` | 所有界面的默认值。`false`（默认）＝关掉 OreUI |
| `screens` | 按界面覆盖，键为路由串 |

```json
{
    "screens": {
        "/main-menu": false,
        "/settings": true
    }
}
```

可用界面名即运行日志里被改写的 key。1.26.62.1 共 30 个，包含 `/main-menu`、
`/play`、`/settings`、`/edit-world`、`/safezone`、`/create-new-world`、
`/player-permissions`、`/send-invites`、`/pre-start`、`/oreui-marketplace/search` 等。

改完存盘即生效，无需重启。

## 诊断

```bash
CLOSEOREUI_DEBUG=1 mcpelauncher-client \
  -dg ~/.local/share/mcpelauncher/versions/<版本> \
  -dd ~/.local/share/mcpelauncher \
  -m ~/.local/share/mcpelauncher/mods/mcpelauncher-updates/<ver>/x86_64/
```

正常输出：

```
CloseOreUI preinit (mod 1.0.0, target linux-x86_64/x86_64)
CloseOreUI init (mod 1.0.0)
running game: 1.26.62.1 (versionCode=982606201)
function table: 768478 entries
OreUI config function at 0x... (top-level screens=10, routes=14, candidates=441)
hook installed: target=0x... prologue=6 (rel32)
first application: 30 screens patched
```

`running game` **仅用于诊断**。模组不记录、也不比较构建时的游戏版本——定位基于结构，
同一份二进制通常能在相邻的 patch 版本上工作，这是本项目的设计前提。

> 手动用 `mcpelauncher-client` 启动时，`-m` 必须带上
> `mods/mcpelauncher-updates/<版本>/x86_64/`，否则会因缺少补丁库报
> `cannot locate symbol "pthread_sigmask"` 而崩溃。通过启动器启动会自动处理。

## 发布

`.github/workflows/release.yml` 有两种触发方式：

| 触发 | 版本号来源 | 用途 |
|---|---|---|
| 推送 tag `v*` | 从 tag 解析 | 常规发布 |
| 手动触发 | `git describe` | 需要额外标注实测版本时 |

手动触发时可填 `tested_with`（例如 `1.26.62.1`），它**只出现在 release 说明里**，
不会写进二进制，也不影响文件名。文件名固定为
`closeoreui-<版本>-linux-x86_64.tar.gz`。

### CI 在这里的作用

CI 的存在意义是**可信的公开构建**：release 里的 `.so` 由公开源码在公开 runner 上编译，
而不是某台机器上的产物。对一个要 hook 进游戏内存的模组，这一点比便利性重要。

**它不是兼容性检查。** runner 上没有游戏文件，也不该有。所以"workflow 通过"只意味着
「能编译、ABI 表面符合预期」，从不意味着「在你的版本上能用」。兼容性由模组运行时日志报告。

工作流强制的检查：

```
host 测试 → 编译 → 校验导出 mod_init / mod_preinit
         → 校验 NEEDED 只有启动器/游戏提供的库
         → 打包 .so + README + SHA256
```

全程**不接触、不缓存、不发布任何游戏文件**。

## 跨版本适配能力

本项目**不硬编码任何地址**，每次启动现场推导目标函数位置。但适配能力分层，不能一概而论。

### 能自动适配

| 项 | 原因 |
|---|---|
| 函数地址 | 每次启动重新计算，不存储 |
| 路由列表 | 不硬编码任何路由字符串，某版本增删界面不影响 |
| 目标选择 | 基于「注册函数必然覆盖所有顶层界面」这一结构事实 |

### 可能适配，但需观察

| 项 | 风险 |
|---|---|
| 判据余量 | 已测的 5 个版本（1.26.50.4 ~ 1.26.62.1）第一名为 10 条顶层路由，第二名都是 7 条（全部是 `/realms-*` 子模块），余量稳定。但若未来版本给 realms 增加 4 个以上顶层路由，第二名会追平 |
| 路由串的引用形式 | 若某版本把路由表合并进 `.data.rel.ro` 的指针数组（编译器合并字符串的常见结果），定位失效。此时会**明确报错退出**，不会乱改内存 |

### 不适配，需人工确认

这三处是硬约束，依赖上游逆向结论继续成立：

| 项 | 依赖 |
|---|---|
| `OreUIConfig` 布局 | 四个字段的顺序与类型不变 |
| detour 签名 | 参数个数不变（1.26.62.1 上实际使用 5 个寄存器） |
| `std::function` ABI | 与游戏同一套 libc++，`_LIBCPP_ABI_*` 开关一致 |

因此准确的说法是：**地址定位层是自动的，行为层不是**。跨版本时若日志显示定位成功但界面未变化，
应优先怀疑上表三项。

## 验证情况

以下版本在 Linux x86_64 上实测通过（启动游戏，日志确认 `applied OreUI config to N screens`
且界面回到旧 UI）：

| Minecraft 版本 | 定位地址 | 顶层界面 | 改写界面数 |
|---|---|---|---|
| 1.26.50.4 | `base+0xa689b80` | 10 | 28 |
| 1.26.51.1 | `base+0xa689c00` | 10 | 28 |
| 1.26.52.3 | `base+0xa689d50` | 10 | 28 |
| 1.26.60.4 | `base+0xaa73870` | 10 | 30 |
| 1.26.62.1 | `base+0xaa73a10` | 10 | 30 |

每一版的候选余量一致：第一名 10 个顶层界面，第二名都是 7 个（全部是 `/realms-*` 子模块）。

1.26.62.1 还额外用 Ghidra 12.2 反编译交叉验证过：`entry_matches=true`，序言为
`push rbp/r15/r14/r13/r12/rbx` + `sub rsp,0x178`，函数体是 `string::__init`
→ 构造两个 `std::function` → 插入 map，确实是配置注册函数。

其他版本尚未验证。定位依赖「路由串被 `lea` 直接引用」这一结构；若某版本把
路由表合并进 `.data.rel.ro` 的指针数组，定位会失败并明确报错退出，不会乱改内存。

## 工具

```bash
# 不开游戏，离线复现定位算法（判据与运行时一致）
python3 tools/analyze_so.py ~/.local/share/mcpelauncher/versions/<版本>/lib/x86_64/libminecraftpe.so
```

`tools/ghidra/` 下有两个脚本，用于独立验证某个地址是否真的是 OreUI 配置函数：

- `VerifyOreUiFunction.java` — 检查函数入口、引用路由数、调用者、ABI 形状
- `DecompileAt.java` — 强制反汇编并反编译指定地址，输出引用的字符串

## 测试

主机侧测试不依赖 NDK：

```bash
./tests/run_tests.sh
```

`discovery_test` 构造带干扰项的二进制（12 条路由的真目标、7 条路由的密集干扰项、
6 个单路由小函数），验证定位选中正确的那个。

## 已知限制

- 只支持 **x86_64**。ARM 请用 ForceCloseOreUI。
- 只支持 mcpelauncher。其他启动器（如 LeviLauncher 的 preload-native）ABI 不同。
- 行为层依赖上游逆向结论继续成立，详见[跨版本适配能力](#跨版本适配能力)。
- 不提供任何游戏文件，也不修改 APK。
