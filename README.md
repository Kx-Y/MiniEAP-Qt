MiniEAP
=======

这是一个实现了标准 EAP-MD5-Challenge 算法的 EAP 客户端，支持通过插件来修改标准数据包以通过特殊服务端的认证。目前带有一个实现锐捷 v3 (v4) 算法的插件。本插件的认证算法来自 [Hu Yunrui 的 MentoHUST 项目](https://github.com/hyrathb/mentohust)，在此表示感谢！

> 本仓库是 [updateing/minieap](https://github.com/updateing/minieap) 的 **Qt 图形界面分支**，
> 新增了跨平台 GUI、Windows 原生支持，并修复了若干在 Windows 上阻止认证成功的问题。
> 修复清单见 [docs/FIXES.md](docs/FIXES.md)。

## 图形界面（Qt 6，跨平台）

本项目附带一个基于 **Qt 6** 的图形界面，可用于 **Windows / Linux / macOS**，
它复用同一个认证引擎，原命令行客户端的行为保持不变。

![主界面](docs/screenshot-main.png)

- 多配置管理、网卡自动探测、实时彩色日志（密码打码）、连接状态一目了然
- 一键连接 / 断开，认证在后台线程运行，界面不卡顿；支持反复断开重连
- 原有的 `minieap` 命令行、`minieap.conf` 与 systemd 用法全部保留

### 快速开始

| 平台 | 构建 | 运行 |
|---|---|---|
| Windows | `scripts/build-windows.ps1` 或 CMake | `build-win\minieap-qt.exe` |
| Linux | `cmake -S . -B build && cmake --build build -j` | `sudo ./build/bin/minieap-qt` |
| macOS | 同上（`brew install qt`） | `sudo ./build/bin/minieap-qt` |

```bash
# CMake：一次构建出命令行客户端与图形界面
cmake -S . -B build && cmake --build build

# 或者只构建原来的命令行客户端
make
```

Windows 下推荐 `scripts/build-windows.ps1`：它会用 Qt 自带的 llvm-mingw 编译
（Qt 6 的 Windows 包基于 libc++，必须用匹配的工具链），并把 Qt 运行时 DLL
一并复制到输出目录。

### 运行时依赖

| 平台 | 抓包模块 | 要求 |
|---|---|---|
| Windows | `npcap` | 安装 [Npcap](https://npcap.com)；普通用户使用需**取消**"仅限管理员访问"，否则以管理员身份运行 |
| Linux | `sockraw`（默认） | root 或 `setcap cap_net_raw,cap_net_admin=eip <程序>` |
| macOS | `bpf` | root |

**完整文档**：[docs/QT-GUI.md](docs/QT-GUI.md)（构建、平台细节、配置说明、故障排查）  
**修复记录**：[docs/FIXES.md](docs/FIXES.md)（Windows 平台移植与认证失败的根因分析）  
**对上游的改动**：[docs/UPSTREAM-CHANGES.md](docs/UPSTREAM-CHANGES.md)（逐文件说明改了什么、为什么）

## 特性

#### 通用特性

* 模块化设计
可在 `config.mk` 中直观选择所需模块。如需添加模块，直接复制一份现有的 `minieap.mk` 并按需修改即可。

* 网络帧收发由插件模块完成
可根据平台差异使用不同的插件。目前提供 `libpcap` 和 Raw Socket 两种插件。前者兼容性好，但需链接 `libpcap`，体积较大；后者不需额外动态库，但只能在 Linux 上使用。可选择任意个模块参与编译，但运行时只能选取其中之一来使用。

* 数据包修改同样由插件完成
可以在不修改主要认证流程的情况下适配各种环境。可以启用多个插件，也可将一个插件启用多次。程序会让标准 EAP 算法生成的数据包按照命令行中 `--module` 参数的顺序让数据包流经这些插件。目前提供一个锐捷 v3 认证算法插件和一个打印流经的数据包大小的示例插件。

* 所有数据包生成逻辑均采用结构体对缓冲区进行读写，拒绝 magic number 从我做起！

#### 锐捷插件特性

* 认证算法来自 [Hu Yunrui 的 MentoHUST 项目](https://github.com/hyrathb/mentohust)
* 相比原本的 MentoHUST v3 (v4) 实现，能够支持更多的字段，更容易通过验证。
* 二次认证时，支持位于修改常规字段以外的 IP 地址、网关、主 DNS 等信息，更容易通过验证。
* 所有字段都通过收集来的信息直接构造而成，不采用修改数据包模板的方式，避免各场景下偏移量不同导致的认证失败或数据包无法解析问题。
* 所有字段生成逻辑均采用结构体对缓冲区进行读写，拒绝 magic number 从我做起 x2！
* 字段中所用到的常量都有宏定义来注明其含义，定长字段的长度也通过宏定义声明，拒绝 magic number 从我做起 x3！
* 支持通过命令行来附加新的字段，也可覆盖程序生成的字段。可以在不修改代码的情况下进行适配。
* 整体程序的内存占用比 MentoHUST 小约 78%（在 256 MB 内存的 ARMv7 平台上测试）。

## 编译

推荐使用 CMake，一次构建可以在 Windows / Linux / macOS 上得到命令行客户端与
图形界面：

```bash
cmake -S . -B build && cmake --build build
# 产物：build/bin/minieap、build/bin/minieap-qt
```

只想要原来的命令行客户端时，下面的 `make` 方式与以前完全一致。

1. 编辑 `config.mk`，选择所需要的模块。
在以 `if_impl` 开头的模块中，Linux 环境建议只选择 `if_impl_sockraw` 模块，其他平台建议只选择 `if_impl_libpcap` 模块。
在以 `packet_plugin` 开头的模块中，请按需要选择。
注：若选择 `if_impl_libpcap`，将自动添加 `-lpcap` 选项。

3. 如果服务器消息乱码，可将 `config.mk` 中的 `ENABLE_ICONV` 置为 1.
若平台未提供iconv相关函数，需手动链接 `libiconv` 库。

4. 执行 `make` 即可在根目录下编译出可执行文件。

注1：如需要交叉编译，可参考 `config.mk` 中的示例。

注2：如需要链接外部库，请在 `COMMON_CFLAGS`、`COMMON_LDFLAGS`、`LIBS` 中加入合适的 `-I -L -l` 等选项。

## 运行

具体选项请参阅 `minieap -h` 的输出。这里列出必需的几个选项。

* `-u 用户名`
* `-p 密码`
* `-n 网卡名`

默认的网络帧收发模块是 `if_impl_sockraw`。如果要使用其他模块，如 `libpcap`，则必须指定 `--if-impl libpcap`。

默认不使用任何数据包修改器，将只会发送单纯的标准 EAP 数据包。 **如需使用锐捷认证，则必须指定 `--module rjv3`。** 可以指定多个 `--module` 参数，程序会按参数的顺序让数据包流经这些插件。

参数格式支持如下几种：

* `-u myname`
* `--username myname`

注意：暂不支持 `-umyname` 这种形式，这在插件的命令行解析中将带来错误。

示例：在 en0 上使用锐捷认证，以 `libpcap` 作为网络帧收发模块，并且在数据包流经锐捷认证插件前后都打印出数据包的大小：

```
minieap -u 201000000 -p 15000000000 -n en0 --module printer --module rjv3 --module printer --if-impl libpcap
```

## 注意事项

本项目刚成立不久，虽然有过测试，但无法保证高可靠性。欢迎大家提出意见，谢谢！

非常感谢 HustMoon 工作室以及 Hu Yunrui 同学对这个领域做出的贡献！
