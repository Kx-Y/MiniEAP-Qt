# Windows 平台移植与修复记录

本文记录把 MiniEAP 移植到 Windows 并使其真正完成 802.1X 认证的过程中，
定位到的问题与修复。每条都给出了根因和验证方法，便于日后回归。

原始 MiniEAP 只支持 Linux / macOS。图形界面分支新增了 Windows 支持，
但"能编译"与"能认证"之间还隔着 10 个问题。

---

## 一、崩溃类

### 1. `0xc0000028` 栈损坏崩溃

**现象**：认证进行到中途，进程崩溃，错误码 `STATUS_BAD_STACK`。

**根因**：`util/sched_alarm.c` 在 Windows 上用独立线程模拟 `SIGALRM`，
定时器线程**直接执行**了用户的闹钟回调：

```c
/* 修复前 */
if (_res == WAIT_TIMEOUT) {
    alarm_lock();
    alarm_ring();     /* ← 在定时器线程里执行回调 */
    alarm_unlock();
}
```

而回调（`state_watchdog`）会调用 `switch_to_state()`，修改状态机全局量、
写日志、甚至发送以太网帧——这些都只允许在引擎线程上做。两个线程同时
操作同一份状态，破坏了栈。

**修复**：定时器线程只负责"把闹钟标记为到期"，回调改由引擎线程在
`sched_alarm_poll()` 中执行：

```c
/* 定时器线程 */
InterlockedExchange(&g_alarm_expired, _secs);

/* 引擎线程，在接收超时处调用 */
void sched_alarm_poll() {
    LONG _secs = InterlockedExchange(&g_alarm_expired, 0);
    if (_secs > 0) { alarm_ring(); }
}
```

这与 POSIX 语义一致：`SIGALRM` 本来就投递到引擎线程，打断它的 `recv()`。

---

## 二、编码类

### 2. 中文网卡名乱码，导致"找不到网络适配器"

**现象**：Linux 下 `nic=eth0` 可用，Windows 下 `nic=以太网` 报找不到适配器。

**根因**（共 3 处，都是 UTF-8 与 GBK 混用）：

1. **GUI → 引擎**：`QString::toLocal8Bit()` 在 Qt 中按 C locale（UTF-8）转换，
   而引擎的 `char*` 字面量和 Win32 API 得到的是系统 ANSI 代码页（中文系统为 GBK）。
   引擎拿到 UTF-8 字节，却去和 GBK 字符串比较，永远不等。
2. **Npcap 设备名解析**：同样先试 UTF-8 再试 CP_ACP，顺序错误。
3. **网卡查询**：`GetAdaptersAddresses` 返回宽字符，转窄字符时同样用错了代码页。

**修复**：统一使用 **CP_ACP**（`gui/enginebridge.cpp` 的 `encodeForEngine()`、
`util/npcap_device_name.c`、`util/net_util_win32.c`）。

> 注：GUI 显示的日志解码则相反——先试 UTF-8 再退回 `fromLocal8Bit`，
> 因为引擎输出可能是两者之一。

### 3. Npcap 只接受 `\Device\NPF_{GUID}`，不接受友好名称

**现象**：`pcap_open_live("以太网", ...)` 返回
`Error opening adapter ... (123)`。

**根因**：Npcap 的设备名是 `\Device\NPF_{GUID}`，友好名称（"以太网"）
是 Windows 显示名，Npcap 不认。

**修复**：新增 `util/npcap_device_name.c`，把友好名称/描述/GUID 解析为设备路径；
`if_impl/npcap/if_impl_npcap.c` 在 `set_ifname()` 时完成解析。

```c
if (!minieap_win32_npcap_device_name(PRIV->ifname, PRIV->device, IFNAMSIZ)) {
    strncpy(PRIV->device, PRIV->ifname, IFNAMSIZ - 1);  /* 失败时让 Npcap 报错 */
}
```

---

## 三、协议类（这些是"能编译但不能认证"的真正原因）

### 4. 源 MAC 错误：查到了第一块网卡而非选中的网卡

**现象**：认证帧从 `vEthernet (Default Switch)` 的 MAC 发出，而非"以太网"的 MAC；
交换机按端口 MAC 校验，直接丢弃。

**根因**：`GetAdaptersAddresses()` 返回**一整块内存里的链表**，
匹配到的网卡往往不是链表头（Hyper-V 的 vEthernet 常排第一）：

```c
/* 修复前 */
if (_match) {
    *out = _addrs;      /* ← 返回链表头，而不是 _curr */
    return SUCCESS;
}
```

调用方读 `(*out)->PhysicalAddress`，拿到的是第一块网卡的 MAC。

**修复**：深拷贝匹配到的节点，由调用方用 `win32_free_adapter_copy()` 释放：

```c
IP_ADAPTER_ADDRESSES* _copy = calloc(1, sizeof(*_copy));
memmove(_copy->PhysicalAddress, _curr->PhysicalAddress, ...);
_copy->AdapterName = _strdup(_curr->AdapterName);
*out = _copy;
```

**验证**（抓包对比）：

```
修复前： src=00:15:5D:E4:B9:49   （vEthernet）
修复后： src=E8:9C:25:D0:B6:E6   （以太网，与锐捷客户端一致）
```

### 5. 缺少 EAPOL-Logoff，服务器不响应

**现象**：一直"正在查找认证服务器"，服务器零响应。

**根因**：抓包对比官方锐捷客户端，它的第一个动作是发 **3 个 EAPOL-Logoff**：

| 帧 | 方向 | 内容 |
|---|---|---|
| 1-3 | 发出 | Logoff（18 字节）|
| 4 | **收到** | EAP-Request/Identity ← 服务器主动发起 |
| 5-7 | 发出 | Start（588 字节，发往 `01:D0:F8`）|

交换机若认为该 MAC 还有活动会话，**不会响应新的 EAPOL-Start**。
只有 Logoff 能重置端口的 802.1X 状态机。

**修复**：在 `eap_state_machine.c` 首次进入 `START_SENT` 前发送 3 个 Logoff：

```c
if (!g_priv.logoff_sent) {
    g_priv.logoff_sent = TRUE;
    for (i = 0; i < 3; ++i) state_mach_send_eapol_simple(EAPOL_LOGOFF);
}
```

### 6. rjv3 插件给 Logoff/Start 追加私有字段，导致被丢弃

**现象**：Logoff/Start 变成了 522 字节，交换机不认。

**根因**：`rjv3_append_priv()` 对所有帧无条件追加锐捷私有字段。
但 Logoff/Start 没有 EAP 载荷，官方客户端发的是**干净的 18 字节**。

**修复**：只对 `EAP_PACKET` 追加：

```c
if (frame->header->eapol_hdr.type[0] != EAP_PACKET) {
    rjv3_apply_bcast_addr(this, frame);
    return SUCCESS;
}
```

### 7. Npcap 的 BPF 过滤器丢弃了入站 EAPOL 帧 ⭐ 最难定位

**现象**：所有帧都正确发出，但 `pcap_next_ex()` 永远收不到服务器的响应。

**根因**：过滤器 `ether proto 0x888e` 在部分网卡上会过滤掉**入方向**帧。
用独立抓包工具能看到服务器帧，程序自己却看不到——这是判断关键。

**修复**：不再安装内核过滤器，改在用户态过滤（引擎本来就只处理 EAPOL）：

```c
if (_hdr->caplen < 18 || _data[12] != 0x88 || _data[13] != 0x8E) {
    continue;   /* 非 EAPOL，丢弃 */
}
```

> 保留 `MINIEAP_PCAP_FILTER=1` 环境变量用于对比诊断。

---

## 四、GUI 与引擎协作类

### 8. 重复启动引擎：`optind` 未重置，参数被全部忽略 ⭐

**现象**：断开后再次连接，日志显示命令行参数完全正确，
引擎却报"用户名不能为空"。

**根因**：`getopt_long()` 用 **libc 全局变量 `optind`** 记录解析位置。
第一次连接解析完后 `optind` 停在 `argc`；第二次调用时 `getopt_long()`
立刻返回 -1，**循环一次都不执行**，所有参数被跳过。

```c
/* 修复前：没有重置 */
opt = getopt_long(argc, argv, shortOpts, longOpts, &longIndex);

/* 修复后 */
#ifdef __GLIBC__
    optind = 0;   /* glibc 需要 0 才能完全重新初始化 */
#else
    optind = 1;
#endif
```

**回归测试**（`%TEMP%\miopt\optind-test.c`）：

```
round 1: user=user1 pass=pass1 nic=eth0
round 2: user=user2 pass=pass2 nic=eth1
PASS: both rounds parsed
```

### 9. 全局配置在上次运行后悬空

**根因**：`load_default_params()` 只重新赋值部分字段；`free_config()` 释放了
`conffile`/`username`/`ifname`，第二次运行这些指针是悬空的。

**修复**：`load_default_params()` 开头整体清零。

### 10. GUI 状态栏不更新 / 无法输入 0

| 问题 | 根因 | 修复 |
|---|---|---|
| 认证成功仍显示"未连接" | `containsAny()` 用 `QLatin1String("认证成功")` 把 UTF-8 字节当 Latin-1 比 | 改 `QString::fromUtf8()` |
| 心跳日志把状态打回"正在认证" | 状态判断顺序问题 | 已连接后忽略心跳日志 |
| **阶段超时/重试次数无法设为 0** | `setRange(1, ...)` 把 0 钳到 1 | `setRange(0, ...)` + `setSpecialValueText("0（不超时）")` |
| 自动提权导致挂起 | UAC 被策略禁用时 `ShellExecuteEx(runas)` 不返回 | 改为仅 `--elevate` 时提权 |

最后一条值得展开：为了能抓包，曾让程序自动请求管理员权限。但在
**UAC 提示被策略关闭**的机器上，`ShellExecuteEx` 既不弹窗也不启动子进程
也不返回，表现为"进程卡住、日志为空、参数丢失"。默认不再自动提权。

---

## 五、配置要点（实测可用）

```ini
username=yaokx5
password=<密码>
nic=以太网              # Windows 用友好名称；Linux 用 enp3s0
module=rjv3
fake-dns2=0.0.0.0
fake-serial=0
stage-timeout=0         # 0 = 永不超时（校园网认证必需）
max-retries=0           # 0 = 不限重试次数
```

`stage-timeout` 与 `max-retries` 设为 0 是关键：服务器在"回应密码请求"后
需要 1~3 秒返回结果，默认的 5 秒 / 3 次在部分网络下不够；
设为 0 则一直等待。

---

## 六、验证结果

```
[I] 正在查找认证服务器
[I] 已发送 EAPOL-Logoff 清除旧会话
[I] 正在回应用户名请求
[I] 正在回应密码请求
[I] 认证成功                     ← ✅
```

网络状态：获得 IP `172.18.38.10/22`，网关可达，公网 HTTP 200。

## 七、已知问题

- 日志出现 `无法找到 echo key 的位置，将不能进行心跳`：rjv3 未能解析
  锐捷私有心跳字段。不影响认证与在线保持（服务器会周期性下发
  Request/Identity，引擎正常响应）。
