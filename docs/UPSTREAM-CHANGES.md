# 对上游代码的改动说明

本分支基于 [updateing/minieap](https://github.com/updateing/minieap) @ `983fd4a`。
原则是**尽量不改动上游代码结构**：新功能放在新文件里，对上游文件的修改
仅限于「平台适配」和「认证必需」两类，且每处都保持原有语义。

本文逐文件说明改动内容、原因，以及为什么无法用新增文件替代。

---

## 一、改动总览

| 类别 | 文件数 | 说明 |
|---|---|---|
| **新增文件** | 41 | GUI、桥接层、Windows 实现，不触碰上游 |
| **平台适配**（`#ifdef` 包裹） | 12 | POSIX 逻辑保持不变，仅补 Windows 分支 |
| **功能增补**（向后兼容） | 6 | 日志钩子、退出原因、可复用入口 |
| **缺陷修复**（必需） | 5 | 不修则认证无法成功 |
| **构建** | 3 | Makefile 增加 GUI 目标、CMake、gitignore |

---

## 二、缺陷修复类（无法避免）

### 1. `config.c` — 重置 getopt 的 `optind`

```c
+ #ifdef __GLIBC__
+     optind = 0;
+ #else
+     optind = 1;
+ #endif
```

**原因**：`getopt_long()` 的解析位置存在 libc 全局变量 `optind` 里。
GUI 在同一进程内多次启动引擎时，第二次调用会因 `optind` 已到 `argc`
而**跳过全部参数**，表现为"用户名不能为空"。

**为什么不能放新文件**：`parse_cmdline_opts()` 是上游函数，重置必须发生在
它的入口处。改动 4 行，原有逻辑一行未动。

### 2. `packet_plugin/rjv3/packet_plugin_rjv3_priv.c` — Start/Logoff 不加私有字段

```c
+ if (frame->header->eapol_hdr.type[0] != EAP_PACKET) {
+     rjv3_apply_bcast_addr(this, frame);
+     return SUCCESS;
+ }
```

**原因**：抓包对比官方锐捷客户端，其 Logoff/Start 是 **18 字节**的纯 EAPOL 帧；
本插件给它们追加了约 500 字节私有字段（变成 522 字节），交换机直接丢弃。

**为什么不能放新文件**：这是插件内部的数据包构造逻辑，改动 4 行。

### 3. `eap_state_machine.c` — 首次启动前发送 EAPOL-Logoff

```c
+ if (!g_priv.logoff_sent) {
+     g_priv.logoff_sent = TRUE;
+     for (i = 0; i < 3; ++i) state_mach_send_eapol_simple(EAPOL_LOGOFF);
+ }
```

**原因**：官方客户端启动时先发 3 个 Logoff 清除旧会话，之后服务器才会
主动下发 Request/Identity。缺少这一步，交换机认为该 MAC 仍有活动会话，
**不响应任何 Start**——这是"一直卡在查找认证服务器"的原因之一。

**为什么不能放新文件**：状态机的发送时机在 `trans_to_start_sent()` 内部。

### 4. `if_impl/if_impl.c`、`packet_plugin/packet_plugin.c` — Windows 模块注册表

```c
+ #ifdef _WIN32
+ static IF_IMPL* (*const g_if_impl_registry[])() = { ... };
+ #endif
```

**原因**：上游用 ELF 的 `__start_/__stop_` 符号遍历自定义段来发现模块，
**Windows PE 格式没有这个机制**（已实测 MSYS2 ld 与 lld 均不支持）。
Windows 分支改用显式注册表，POSIX 分支保持原样。

### 5. `util/net_util_win32.c` — 网卡查找返回正确节点

新增文件，但其中一处逻辑值得说明：`GetAdaptersAddresses()` 返回**一整块内存里
的链表**，匹配到的网卡通常不是链表头。早期实现返回链表头，导致发出的帧
携带了错误的源 MAC（`00:15:5D:...` 而非网卡的真实 MAC），交换机按端口 MAC
校验后丢弃。现改为深拷贝匹配节点。

---

## 三、平台适配类（`#ifdef` 包裹，POSIX 行为不变）

这些改动全部在 `#ifdef _WIN32` 内，上游在 Linux/macOS 上的行为完全没有变化。

| 文件 | Windows 分支 | POSIX 分支（原样保留） |
|---|---|---|
| `util/sched_alarm.c` | 定时器线程 + `sched_alarm_poll()` | `alarm()` + `SIGALRM` |
| `util/pid_lock.c` | 命名互斥体 | `flock()` + PID 文件 |
| `util/misc.c` | `MultiByteToWideChar`(CP936) 转码、无 `fork()` | `iconv`、`fork()`+`setsid()` |
| `util/logging.c` | `CRITICAL_SECTION` 串行化 | `pthread_mutex` |
| `include/types.h` | 用 Win32 类型（避免与 `windows.h` 冲突） | 原定义 |
| `include/module_init.h` | 段注册宏展开为空 | `__attribute__((section))` |
| `include/platform.h`（新增） | 统一定义 `_WIN32_WINNT`、`IFNAMSIZ`、`strndup` 等 | 引入 `net/if.h` |
| `packet_builder.c` | `winsock2.h` 代替 `arpa/inet.h` | 原样 |
| `packet_plugin/rjv3/packet_plugin_rjv3*.c` | `winsock2.h`；`_spawnvp` 代替 `system()` | 原样 |
| `if_impl/libpcap/if_impl_libpcap.c` | 可用 Npcap SDK 头文件 | 原样 |

---

## 四、功能增补类（向后兼容，不影响原有行为）

### 1. `util/logging.c` — 日志钩子

新增 `g_minieap_log_hook`，让 GUI 直接收到引擎输出的每一行，
**不必解析 stdout**。钩子为 NULL 时行为与上游完全一致。

顺带修复：`g_time_buffer[20]` 太小触发 `-Wformat-truncation`，改为 64。

### 2. `include/minieap_runtime.h`、`util/runtime.c`（新增）

定义日志钩子、退出原因、停止请求标志。
`engine_exit()` 替换了 `eap_state_machine.c` 里的裸 `exit()`，
在调用 `exit()` **之前**多记录一个退出原因——退出码与上游一致。

### 3. `minieap.c` — 把 `main()` 拆成可复用入口

```
原：  main() { srand; atexit; 初始化; switch_to_state(PREPARING); }
新：  minieap_engine_init(argc, argv, install_signal_handlers)
      minieap_engine_run()          /* = switch_to_state(PREPARING) */
      minieap_engine_cleanup()
main() 保留，内部按原顺序调用以上三步
```

**原因**：GUI 需要在工作线程里反复启动/停止引擎。**调用顺序和每一步的内容
与上游完全一致**，只是从 `main()` 里提出来。

命令行客户端走了同一个路径（`main()` 调用同一组函数），行为无变化：
`--help` 退出码 0、认证失败退出码 -1、`atexit` 打印"MiniEAP 已退出"均已实测。

### 4. `util/conf_parser.c` — 容忍 CRLF

```c
+ while (line_len > 0 && (line_buf[line_len-1]=='\r' || line_buf[line_len-1]=='\n'))
+     line_buf[--line_len] = 0;
```

**原因**：Windows 上用户用记事本保存配置会产生 `\r\n`，原代码会把 `\r`
留在值末尾。仅在 Windows 场景触发，POSIX 文件用 LF 时行为不变。

### 5. `Makefile` — 增加 GUI 目标

原 `minieap` 目标**完全未改**，仅追加 `gui` / `gui-clean` 目标。

---

## 五、曾改动但已回退的部分

初版还在 `load_default_params()` 加了 `memset` 清零、在 `conf_parser_free()`
里把 `g_conf_file` 置 NULL。经对照验证：

- `chk_free()` 本来就会把指针置 NULL，`free_config()` 覆盖了所有需要释放的字段
- `conf_parser_set_file_path()` 每次初始化都会重新赋值

因此这两处是**冗余的防御性改动**，已回退。真正修复重复启动问题的是
`config.c` 的 `optind` 重置。

可以用 `scripts/optind-test`（见 docs/FIXES.md）验证。

---

## 六、未改动的上游部分

以下核心逻辑**一行未改**：

- `md5.c`、`packet_builder.c`（除头文件平台适配）—— EAP 报文构造
- `packet_plugin/rjv3/rjv3_hashes/` —— 锐捷算法（除 `checkV4.c` 的 C90 声明位置）
- `eap_state_machine.c` 的状态转换表与各转换函数逻辑
- `config.c` 的参数解析与校验规则
- `util/linkedlist.c`、`util/packet_util.c`、`util/gbconv.c`
- `if_impl/sockraw/`、`if_impl/bpf/` —— POSIX 抓包实现

---

## 七、代码规模

```
上游文件修改：  约 1000 行（含注释）
新增文件：      约 4500 行
其中 GUI：      2347 行
     桥接层：    628 行
     Windows：   约 1500 行
```
