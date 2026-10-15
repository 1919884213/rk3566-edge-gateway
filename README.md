<div align="center">

# rk3566-edge-gateway

**基于 epoll Reactor 的 Linux 边缘协议网关 —— 下行 CAN/Modbus 采集 + 上行 MQTT + 板上 LVGL 面板**

`~5700 行 C` · `epoll ET + EPOLLONESHOT` · `固定 worker 线程池` · `SocketCAN` · `Modbus RTU` · `自研 MQTT 3.1.1` · `Linux 字符设备驱动` · `LVGL 9.6` · `CMake`

[![OS](https://img.shields.io/badge/OS-Linux-informational?style=flat-square)](#架构)
[![Lang](https://img.shields.io/badge/Lang-C-blue?style=flat-square)](#目录结构)
[![IO](https://img.shields.io/badge/IO-epoll%20ET%20%2B%20ONESHOT-orange?style=flat-square)](#架构)
[![Downlink](https://img.shields.io/badge/downlink-SocketCAN%20%7C%20Modbus%20RTU-critical?style=flat-square)](#下行一socketcan)
[![Uplink](https://img.shields.io/badge/uplink-MQTT%203.1.1-yellow?style=flat-square)](#上行mqtt)
[![Kernel](https://img.shields.io/badge/kernel-chardev%20%2B%20kfifo-blueviolet?style=flat-square)](#内核缓存通道-gwcache)
[![GUI](https://img.shields.io/badge/GUI-LVGL%209.6%20%2B%20fbdev-yellowgreen?style=flat-square)](#lvgl-面板)
[![Build](https://img.shields.io/badge/Build-CMake-green?style=flat-square)](#快速开始)

</div>

---

## 项目简介

现场设备各走私有 CAN / 串口协议，云端平台无法直连。这台网关做三件事：

```
       现场设备                    网关 (RK3566 / aarch64)                     云端
  ┌──────────────┐  CAN     ┌──────────────────────────────────────┐
  │ CAN 节点      │─────────▶│ Downlink/can.c  (SocketCAN)          │
  └──────────────┘          │        │                             │
  ┌──────────────┐  Modbus  │        ▼                             │   MQTT  ┌──────────┐
  │ 从站设备      │─────────▶│  Gateway 核心：设备表 + 分组订阅 + 帧路由│─────────▶│ 云平台    │
  └──────────────┘  RTU     │        │                             │         └──────────┘
  ┌──────────────┐  TCP     │        ├──▶ gwcache.ko (kfifo) ──────┼──▶ LVGL 面板
  │ TCP 设备      │─────────▶│        └──▶ 分组下发（同组订阅者）    │
  └──────────────┘          └──────────────────────────────────────┘
```

- **下行两路 + 一路 TCP 接入**：CAN 走 SocketCAN（**作为事件源进同一个 epoll 实例**），Modbus RTU 走串口轮询线程，TCP 设备直接接入。
- **三种来源在上层收敛**：全部转成统一帧 `DATA <source> <key> <value>`，走**同一条出口** —— 内核旁路缓存 + 分组下发 + MQTT 上报。
- **旁路可观测**：每一条分发帧同时写进自研字符设备 `/dev/gwcache`（kfifo 环形缓冲）。网关进程被 `kill -9` 后，最后一批数据帧依然读得到。
- **服务端核心零第三方依赖**：包括 MQTT 客户端都是自己实现的（只做 CONNECT / PUBLISH QoS0 / PINGREQ / DISCONNECT 四种报文）。

## 验证状态（如实标注）

| 项目 | 状态 | 证据 |
|---|---|---|
| 交叉编译（aarch64） | ✅ 通过 | ARM GNU Toolchain 13.3 `aarch64-none-linux-gnu-gcc`，5 个目标 `-Wall -Wextra` **零警告**；`readelf -d` 仅依赖 `libc.so.6` |
| 纯逻辑自检 | ✅ 12/12 | `gwlogic_test`：Modbus CRC16 + MQTT 变长整数边界值，实测输出见下方 |
| 网关核心功能自检 | ⏳ 需在 Linux 上跑 | `gwtest`（注册/订阅/上报/分发/退订/离线全链路）已编过，未在本次环境执行 |
| 内核模块加载 + 端到端 | ⏳ 待板子复跑 | `scripts/gwcache_accept.sh` / `gwcache_integ.sh` 已适配网关协议，读数待跑 |
| 真机 CAN / Modbus / MQTT 联调 | ⏳ 待做 | 需要 CAN 收发器、Modbus 从站、MQTT broker |

> 上表是**刻意保留**的：这份 README 只写跑过的读数。真机跑完把数字填进来即可。

### `gwlogic_test` 实测输出

```
--- Modbus CRC16 ---
  [PASS] 标准检查值 "123456789" == 0x4B37
  [PASS] FC03 请求帧 CRC == 0x0A84
  [PASS] 线上顺序低字节在前 => 84 0A（与公开参考帧一致）
  [PASS] 任一字节翻转后 CRC 改变
--- MQTT 剩余长度变长整数 ---
  [PASS] 0 -> 1 字节 [00]
  [PASS] 127 -> 1 字节 [7F]（单字节上限）
  [PASS] 128 -> 2 字节 [80 01]（进位点）
  [PASS] 16383 -> 2 字节 [FF 7F]（两字节上限）
  [PASS] 16384 -> 3 字节 [80 80 01]（继续进位）
  [PASS] 2097151 -> 3 字节 [FF FF 7F]（三字节上限）
  [PASS] 2097152 -> 4 字节（MQTT 规范的最大长度边界）

===== 结果: ALL PASS（失败 0 项）=====
```

这两段位运算最难从外部观察（CRC 错 = 从站永远不回帧；变长整数错 = broker 直接掐连接），所以单独抽成不依赖 termios/socket 的纯逻辑自检，任何平台都能原生跑。

## 架构

```
                     主线程 (epoll_wait)
                           │  只做事件分发，不阻塞、不处理业务
      ┌────────────────────┼─────────────────────┐
      │ listenfd 就绪       │ clientfd 就绪        │ CAN fd 就绪
      ▼                    ▼                      ▼
 循环 accept 到 EAGAIN   查连接表找绑定 worker   投递给固定的 worker 0
 round-robin 绑定 worker  pool_submit(...)       （CAN 帧序必须保序）
 注册 EPOLLET|ONESHOT           │                      │
      │                          ▼                      ▼
      │                   worker：非阻塞读到 EAGAIN    worker：
      │                   gw_on_bytes() 行状态机       gwcan_read_frame() 读到 EAGAIN
      │                   → 注册 / SUB / DATA          → gw_dispatch_frame()
      │                          └──────────┬───────────┘
      │                                     ▼
      │                        dispatch_locked()：唯一的出口
      │                   ① gwcache_emit()  ② 分组 fanout  ③ gwup_enqueue()
      └──────────────────────────────────────┘
```

关键设计：

- **单 epoll 主线程 Reactor**：一个线程管理全部事件源，事件分发与业务处理解耦。
- **异构事件源复用同一个 Reactor**：TCP socket 与 SocketCAN fd 进同一个 epoll 实例 —— CAN socket 天然可被 `poll`/`epoll` 纳管，不需要为它单开采集线程（和「设备当 epoll 事件源」是同一套 waitqueue 机制）。
- **每事件源绑定固定 worker**：同一 fd 的读写天然串行；TCP 连接 round-robin 分配，CAN 固定绑 worker 0 以**保持帧序**。
- **`EPOLLONESHOT`**（TCP）：fd 触发一次后自动失活，worker 处理完重新武装，保证同一连接同一时刻只有一个任务在飞。
- **`EPOLLET` 边沿触发**：循环 `read` 直到 `EAGAIN`，一次性读干净内核缓冲区。
- **一把全局锁**保护连接表、订阅关系与统计；分发路径全程持锁串行发送，慢订阅者最多拖 8ms（8 次 × 1ms 重试上限），不会拖死整条分发路径。
- **上行入队在锁外**：避免与上行线程的队列锁形成嵌套。
- **按 fd 行缓冲**：TCP 是字节流，一次 `read` 可能拿到半行或多行；状态机把字节流切成 `\n` 结尾的完整行再解析，超长行（>1KB）整行丢弃防内存滥用。

## 三种下行 + 一种上行

### 下行一：SocketCAN

CAN socket 作为**事件源**进主 epoll，读到的帧转成统一帧后走同一条出口。

- `<linux/can.h>` 原始帧 → 解析 11/29 位 ID、负载 hex
- 内置一张 CAN ID → 信号名映射表（生产环境由 DBC 生成）；**未命中的 ID 一律以 `key=raw` 上报，不静默丢帧** —— 比猜出个信号名诚实
- 错误帧 / 远程帧不进数据通道（错误帧应走内核的错误计数，不该被当成上报值）
- 接口打不开（没接收发器）时自动降级，网关继续以 TCP + Modbus 运行

### 下行二：Modbus RTU

**时间驱动**的轮询源，因此不进 epoll，而是跑独立线程：

- `termios` 配置 8N1/8E1/8O1 + 波特率；`cfmakeraw` 关回显与行缓冲；`TCIOFLUSH` 丢掉上电残留（否则第一帧必然对不上 CRC）
- FC03 读保持寄存器 / FC06 写单寄存器，响应帧校验长度、CRC、从站号、功能码（含 `0x80|fc` 异常响应）
- 响应读取用 `poll` + 超时，半帧继续等 20ms —— 现场掉线/接线错都走超时分支
- 单点连续失败 ≥3 次打告警但**不退出**（现场设备掉线是常态）
- 采集点位表在 `main.c` 里是硬编码演示表，生产环境应由配置文件加载

### TCP 设备接入

设备用行文本协议接入，与 Reactor 天然贴合：

```
连接
<设备ID>           -> 注册（查重）
SUB <分组>         -> 订阅 can / modbus / tcp / *（*=全部）
UNSUB              -> 退订
DATA <键> <值>     -> 上报一条数据（来源自动标为 tcp:<设备ID>）
LIST / STAT        -> 分组与订阅者 / 网关统计
<普通文本>          -> 组内消息（分发给同订阅组的其他连接，即下行命令通道）
QUIT               -> 断开
```

### 上行：MQTT

**自实现的最小 MQTT 3.1.1 客户端**（保住「零第三方依赖」这条卖点）：

- 只实现 CONNECT / PUBLISH(QoS0) / PINGREQ / DISCONNECT + PUBLISH 的剩余长度变长整数编码
- 明确不做的：TLS、QoS1/2、SUBSCRIBE、遗嘱 —— 要 TLS 就换 mosquitto 客户端，那会把「零依赖」一起换掉，所以这是**权衡后的取舍，不是遗漏**
- **生产者/消费者解耦**：分发路径只往环形队列塞一帧（非阻塞），独立上行线程慢慢发。云侧时延/断连不能拖住 Reactor
- 队列满丢最旧并计数；发送失败直接丢弃并重连 —— 遥测是周期量，下一周期自然补齐，不需要重传队列
- 断线指数退避重连（1s → 30s），空闲期发 PINGREQ 保活

## 内核缓存通道 (gwcache)

自研字符设备 `/dev/gwcache`，是网关的**旁路可观测通道**：每分发一条数据帧就写一份进来。

| 能力 | 实现位置 |
|---|---|
| 模块生命周期 + Kbuild | `module_init/exit`、`obj-m` |
| 主次设备号 + `cdev` + 类/设备 | `alloc_chrdev_region`、`cdev_add`、`class_create/device_create`（udev 自动建节点） |
| 设备操作集 | `file_operations`：open / read / write / poll / release / unlocked_ioctl |
| 环形缓冲 | `kfifo` + `kfifo_to_user/from_user`（不自己写 ring） |
| 阻塞读 | 等待队列 `wait_event_interruptible` + `wake_up_interruptible` |
| 就绪通知 | `.poll` 返回 `EPOLLIN`，可被 epoll/select 纳管 |
| 控制面 | `unlocked_ioctl`（`_IO/_IOR` 编码，与用户态共用 `gwcache_uapi.h`）+ sysfs `stat` |
| 参数与引用计数 | `module_param(buf_kb)`；`try_module_get/module_put` → 被占用时 `rmmod` 返回 EBUSY |

**并发策略**：读/写/ioctl/sysfs 全部运行在**进程上下文**，统一用一把可睡眠的 `mutex`。这里没有中断上下文，所以**不需要** `spin_lock_irqsave` —— 真把数据源搬进中断处理函数时才需要换。

**缓冲满的策略**：整条丢弃并计数（`drop`），`write()` 仍返回成功。旁路通道**不能因为自己满了就让业务层重试或阻塞**。

**为什么绕内核一圈**：用户态管道/日志缓冲随进程消失，kfifo 在内核内存里，网关被 `kill -9` 后监控端仍能读走最后 N 条；代价是多一次 `copy_*_user`，所以定位是**可观测性**，不是性能优化。

## LVGL 面板

- **显示**：`lv_linux_fbdev` 直写 `/dev/fb0`（720×1280、32bpp、stride 2880，与 `LV_COLOR_DEPTH 32` 对齐，无需位深转换）
- **触摸**：`lv_evdev` 读 `/dev/input/event1`（电容屏 MT protocol B，由 LVGL 自带驱动处理）
- **取数**：`lv_timer` 每 50ms **非阻塞**读一次 `/dev/gwcache`（不另开线程、不阻塞 UI），按 `\n` 切行贴进 flex 列表；上限 200 行，超了删最旧，自动滚到最新
- **统计**：每秒 `ioctl(GWCACHE_GET_STAT)` 刷「写入/读出/丢弃/缓存」
- **交互**：两个触摸按钮 —— 清空屏幕（连带清驱动缓冲）/ 刷新统计
- 设备被卸载/打不开时自动降级为「未就绪」并持续重试，不崩

### 中文字库

LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` **不能直接用**：实测只覆盖 1456 码位，而本项目文案里的汉字缺 101 个。

用 `lv_font_conv` 自建子集（`gui/src/fonts/`）：正文 16px 收 3536 个常用字，标题 24px 只收界面里真正用到的 8 个字（**不同字号各自收字，省约 4 倍体积**）。字表来源 = 扫真实语料 ∪ 源码字符串字面量 —— **不要凭印象手写 `--symbols`**（第一次手写漏了 `·`、`源`、`…`，屏上直接出方框）。

## 目录结构

```
.
├── main.c                  # 装配：三条通道 + Reactor 启动
├── main.h                  # 全局配置（端口、worker 数、CAN/Modbus/MQTT 参数）
├── frame.h                 # 网关统一数据帧定义（三种来源收敛的契约）
├── Server/                 # TCP 初始化 + epoll Reactor 主循环（含 CAN 事件源）
│   └── gwcache_hook.c      # 内核缓存通道的用户态接入（打开/写入/关闭，失败自动降级）
├── Gateway/                # 网关核心：设备表 + 分组订阅 + 帧路由 + 分发
├── Downlink/               # 下行两路
│   ├── can.c               #   SocketCAN（事件源，进主 epoll）
│   └── modbus.c            #   Modbus RTU 主机（轮询线程）
├── Uplink/                 # 上行 MQTT（最小 3.1.1 客户端 + 发送线程）
├── pool/                   # 线程池：每 worker 独立任务队列，按 index 定向投递
├── Client/                 # 设备模拟器 / 订阅查看器（gwclient）
├── build_test/             # 自检：test_gateway.c（核心全链路）+ test_logic.c（纯逻辑）
├── kernel/gwcache/         # 自研字符设备驱动：kfifo + 阻塞读 + poll + ioctl + sysfs
├── gui/                    # LVGL 9.6 面板（fbdev + evdev）+ 自建中文子集字库
├── tools/                  # GwcacheMon：命令行跟踪缓存流 / 看统计
├── scripts/                # 验收脚本（驱动 12 项 / 端到端）+ udev 权限规则
└── CMakeLists.txt
```

## 快速开始

**依赖**：Linux + gcc + CMake + pthread。网关、客户端、监控端**零第三方库**；只有 LVGL 面板需要 LVGL 源码树。

### 1) 本机（x86_64）跑起来

```bash
cmake -S . -B build && cmake --build build -j8

./build/gwlogic_test                 # 纯逻辑自检，12 项应全过
./build/gwtest                       # 网关核心全链路自检
./build/gwgateway                    # 前台跑，监听 0.0.0.0:8080
./build/gwclient watch tcp           # 另开终端：订阅 tcp 分组看数据
./build/gwclient sim 5               # 5 个模拟设备注册并周期上报
```

> 后台跑网关时加 `stdbuf -o0`，否则 stdout 是块缓冲、日志会全部卡在缓冲区里：
> `stdbuf -o0 -e0 ./build/gwgateway > gw.log 2>&1 &`

### 2) 交叉编译到 RK3566（aarch64）

```bash
GW_CROSS_CC=aarch64-linux-gnu-gcc \
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build-arm64 -j8
readelf -d build-arm64/gwgateway | grep NEEDED    # 应只有 libc.so.6
```

### 3) 打开 CAN 通道

```bash
sudo ip link set can0 up type can bitrate 500000
ip -details link show can0

# 没有真实收发器时，用虚拟 CAN 验证整条链路
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
# 另开终端发一帧：cansend vcan0 100#1122334455667788
```

### 4) Modbus 从站

默认读 `/dev/ttyS3 @9600 8N1`，点位表在 `main.c`（从站 1 的 0x0000/0x0001、从站 2 的 0x0000）。
调试可用 `diagslave` 或 `pymodbus` 起一个软件从站，或把 `GW_MB_DEV` 指向 USB 转 485。

### 5) 上行 MQTT（可选）

环境变量覆盖编译期默认值，不配置时整条上行关闭、网关照常跑：

```bash
sudo apt install mosquitto mosquitto-clients
GW_MQTT_HOST=127.0.0.1 GW_MQTT_TOPIC=gateway/data ./build/gwgateway
mosquitto_sub -t 'gateway/data/#' -v        # 另开终端看上报
```

上报格式：topic = `<前缀>/<source>`，payload = `<key>=<value>`，例如
`gateway/data/can:0x100  speed=1122334455667788`。

### 6) 内核缓存通道（可选，缺了也不影响网关）

```bash
cd kernel/gwcache
make KDIR=<目标板内核树> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# 板子上本地编：make
sudo insmod gwcache.ko buf_kb=64

ls -l /dev/gwcache                        # udev 自动建节点
./build/gwcachemon                        # 跟踪实时数据流
./build/gwcachemon -s                     # 统计：写入/读出/丢弃/缓存
./build/gwcachemon -c                     # 清空缓冲并复位计数
sudo rmmod gwcache                        # 被占用时返回 EBUSY
```

让非 root 用户能用设备（默认节点是 `root:root 0600`）：

```bash
sudo cp scripts/99-gwcache.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo rmmod gwcache && sudo insmod gwcache.ko
```

### 7) LVGL 面板（RK3566 / 720×1280 / fbdev + evdev）

```bash
cd gui
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build-arm64 -j8              # 产物 build-arm64/gw_gui

sudo systemctl stop display-manager        # 桌面让位
GWCACHE_DEV=/dev/gwcache ./build-arm64/gw_gui
```

**没装驱动也能调界面**（FIFO 回放）：

```bash
mkfifo /tmp/fake && GWCACHE_DEV=/tmp/fake ./gw_gui &
printf 'DATA can:0x100 speed 1122334455667788\nDATA modbus:1 flow_rate 1234\n' > /tmp/fake
```

## 测试与验收

```bash
./build/gwlogic_test                       # 纯逻辑：CRC16 + MQTT 变长整数（12 项）
./build/gwtest                             # 核心：注册→订阅→上报→分发→退订→离线
sudo bash scripts/gwcache_accept.sh        # 字符设备功能：12 项
sudo bash scripts/gwcache_integ.sh         # 端到端集成
```

## 设计权衡与已知限制

- **全局锁**：连接表/订阅关系/统计共用一把 mutex，多分组分发吞吐受限于此；per-group 锁是明确的下一步。
- **上行无重传**：MQTT 发送失败直接丢弃，依赖遥测的周期性补齐；要保证不丢就得引 QoS1 + 本地持久化队列。
- **无 TLS**：MQTT 走明文 TCP，内网/测试环境可用，公网部署必须换支持 TLS 的客户端。
- **CAN 不做信号解码**：只做 ID→信号名映射，真做物理量解析要接 DBC 文件。
- **点位表硬编码**：Modbus 采集点位在 `main.c`，生产环境应由配置文件加载。
- **缓存有损**：缓冲满整条丢弃，只为呈现「最近发生了什么」，不保证完整审计。
- **单机部署**：无跨机分发、无持久化；重启即清空（阶段性设计）。
- **面板与网关各自独立部署**（面板读本机设备节点），未做远程拉取。

## 设计问答（FAQ）

**为什么 CAN 进 epoll，Modbus 却开线程？**
事件源与轮询源的差别。CAN socket 天然可 `poll`，来一帧处理一帧，复用同一个 Reactor 最省；Modbus 主机是**时间驱动**的（要按周期主动发请求），把它塞进 epoll 反而要自己造定时器，不如老实跑轮询线程。两种来源在上层收敛到同一个 `gw_dispatch_frame()`。

**为什么用 ET + ONESHOT，而不是水平触发？**
ET 要求一次把数据读干净（循环 read 到 `EAGAIN`）；ONESHOT 让同一连接同一时刻只有一个任务在飞，避免同一 fd 被多 worker 并发处理。代价是每次都要重新武装（`EPOLL_CTL_MOD`）。CAN fd 不用 ONESHOT —— 它恒定绑在同一个 worker 上，天然串行。

**为什么每连接绑定固定 worker，而不是共享任务队列？**
同一 fd 的读写天然串行，不需要额外加锁保护连接内部状态；共享队列会让同一连接的任务并发执行，需要再引入 per-connection 锁。CAN 同理：绑死一个 worker 才有帧序。

**上行为什么用队列 + 独立线程，而不是直接在分发路径里 publish？**
分发路径在 worker 线程里、且持着全局锁；在这里做网络写会把云侧时延直接传导成网关的锁持有时间。队列 + 独立线程把两者彻底解耦，队列满了丢弃并计数。

**为什么 MQTT 自己写而不用 mosquitto 客户端？**
因为「服务端核心零第三方依赖」是这个项目的定位之一。代价是只有 QoS0、没有 TLS —— 这是取舍不是遗漏。要 TLS 就换库，那条卖点一起换掉。

**快照通道为什么用 mutex 不用自旋锁？**
所有路径都在进程上下文，可以睡眠；自旋锁是给不能睡眠的上下文（中断）用的。真搬进中断才换 `spin_lock_irqsave`。

**`poll_wait` 能不能省？**
不能。省了之后这个设备永远不会把等待方挂进等待队列，epoll 收不到唤醒。

**写满为什么返回成功而不是报错？**
旁路通道不能因为自己满了就让业务重试或阻塞；丢弃 + 计数（sysfs/ioctl 可见）更合适。

**为什么不用 Qt 而是 LVGL？**
720×1280 窄屏、要秒起、不依赖桌面合成器，LVGL 直写 framebuffer 更轻；Qt 适合多窗口/复杂控件场景。

## 线程池 API

```c
Threadpool* pool_create(int num);                    // 创建 num 个 worker
int pool_submit(Threadpool* pool, int worker_index,  // 定向投递到指定 worker
                void (*taskfunc)(void*), void* arg);
void pool_destroy(Threadpool* pool);                 // 处理完剩余任务后关闭
```

## 演进方向

- [x] epoll ET + ONESHOT + 固定 worker 线程池
- [x] SocketCAN 作为事件源接入同一 epoll 实例
- [x] Modbus RTU 轮询下行 + 点位表
- [x] 自实现最小 MQTT 3.1.1 上行（队列解耦 + 断线重连）
- [x] 自研字符设备内核缓存通道（kfifo + 阻塞读 + poll + ioctl + sysfs）
- [x] 板载 LVGL 面板 + 自建中文子集字库
- [ ] Modbus 点位表改为配置文件加载（现在是 `main.c` 里的演示表）
- [ ] CAN 接 DBC 文件做物理量解码，而不是只映射信号名
- [ ] 下行写通道：把 `gwmb_write_reg()` 接到组内命令，让网关能反向控制从站
- [ ] 锁分片：全局锁 → per-group 锁，提高多分组并发吞吐
- [ ] 上行 QoS1 + 本地持久化队列（当前靠遥测周期补齐）
- [ ] MQTT over TLS（换 mosquitto 客户端，会牺牲零依赖）

## 许可

[MIT](LICENSE)
