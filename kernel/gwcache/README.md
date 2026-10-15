# gwcache —— 网关数据帧的内核旁路缓存字符设备

`/dev/gwcache` 是网关的**旁路可观测通道**：网关每分发一条数据帧就 `write()` 进来一份，
监控端 / 板载面板 `read()`/`poll()` 取走。缓冲在内核 `kfifo` 里，
**网关进程被 `kill -9` 之后，缓冲里的最后 N 条数据帧依然读得到** ——
这是它相对「直接写日志文件」的唯一实质优势：用户态服务的最后一批状态有了取证点。

## 数据流

```
Gateway.c: dispatch_locked()  ──gwcache_emit()──▶  /dev/gwcache  ──kfifo──▶  gwcachemon / LVGL 面板
                                                      (内核环形缓冲)
```

`open` 身份由 `O_ACCMODE` 决定：`O_WRONLY` = 生产者（网关），`O_RDONLY` = 消费者（监控端/面板）。

## 为什么绕内核一圈（设计权衡）

| 理由 | 说明 |
|---|---|
| 崩溃后可取证 | 用户态管道/日志缓冲随进程消失；kfifo 在内核内存里，网关被 `kill -9` 后监控端仍能读走最后 N 条 |
| 权限隔离 | 设备节点 `0660`（root:video），现场数据不是任何本地进程都能抓；比全局可读的日志文件更可控 |
| 多消费者 + 统一就绪语义 | 面板与命令行监控端可同时读；`.poll` 让设备与 socket 进同一个 epoll（同一套 waitqueue 机制） |
| 代价（要承认） | 多一次 `copy_from_user/copy_to_user`、多一层内核依赖；纯做日志时用户态 ring buffer 更快 —— 定位是可观测性，不是性能优化 |

## 编译与加载

```bash
# 交叉编译（对着目标板内核树）
make KDIR=~/linux-kernel/linux-6.1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# 板上本地编（板子已有内核头）
make
sudo insmod gwcache.ko buf_kb=64
sudo rmmod gwcache          # 有 fd 占用时返回 EBUSY
```

参数 `buf_kb` 是环形缓冲容量（KB），默认 64。

## 用户态接口（`gwcache_uapi.h`，与驱动共用同一份定义）

| 接口 | 说明 |
|---|---|
| `open(O_WRONLY\|O_NONBLOCK)` | 生产者接入；缓冲满时整条丢弃并计数，不阻塞 |
| `read()` | `wait_event_interruptible` 阻塞读，无数据时真阻塞而非忙等 |
| `poll()` | 返回 `EPOLLIN`，可被 epoll/select 纳管 |
| `ioctl(GWCACHE_GET_STAT)` | 取 `struct gwcache_stat`（size/len/in/out/drop） |
| `ioctl(GWCACHE_CLEAR)` | 清空缓冲并复位计数 |
| `/sys/class/gwcache/gwcache/stat` | 同统计的一行文本，脚本可直接读 |

## 并发策略

读 / 写 / ioctl / sysfs 全部运行在**进程上下文**，因此统一用一把 `mutex`（可睡眠）。
这里没有中断上下文，所以**不需要** `spin_lock_irqsave` —— 真把数据源搬进中断处理函数时才需要换。

**缓冲满的策略**：整条丢弃并计数（`drop`），`write()` 仍返回成功。
理由：旁路通道**不能因为自己满了就让业务层重试或阻塞**。
