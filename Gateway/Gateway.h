#ifndef GW_GATEWAY_H
#define GW_GATEWAY_H

#include <stddef.h>

#include "frame.h"

/*
 * 网关核心：设备注册 / 分组订阅 / 帧路由 / 分组分发。
 * 与线程模型解耦 —— Net/Server.c 的 worker 线程把「某个 fd 收到的原始字节」
 * 交给 gw_on_bytes()，本模块内部按连接缓冲、切行、驱动状态机。
 *
 * 三种下行源在这里收敛成同一条路径：
 *   CAN      -> gwcan_read_frame()   -> gw_dispatch_frame()
 *   Modbus   -> 轮询线程             -> gw_dispatch_frame()
 *   TCP 设备 -> gw_on_bytes()"DATA"  -> gw_dispatch_frame()
 * 出口只有一条：写内核旁路缓存(gwcache) + 分组下发 + MQTT 上报。
 *
 * 锁策略：一把全局互斥锁保护连接表、订阅关系与统计计数。
 * 分发路径全程持锁，组内发送天然串行；锁内只做非阻塞 write
 * （极端拥塞下丢弃该条），不会死锁。上行入队放在锁外，避免与
 * 上行线程的队列锁形成嵌套。
 */

/* 初始化各状态表容量（cap = MAX_CONN），在 server_run 前调用 */
void gw_init(int cap);

/* ---- fd -> worker 归属表 ---- */
void gw_fd_assign(int fd, int worker); /* accept 时登记归属 */
int  gw_fd_get(int fd);                /* 查归属，-1 = 空闲 */
void gw_fd_release(int fd);            /* 断开时回收，置 -1 */

/* 新设备接入：置为「等待注册」状态并发送欢迎语（主线程 accept 后调用） */
void gw_on_accept(int fd);

/* 处理来自 fd 的一段原始字节（自动跨次缓冲拼行）。
 * 返回：0 继续；1 要求关闭该连接；-1 fd 未登记（过期事件，忽略） */
int  gw_on_bytes(int fd, const char* buf, size_t len);

/* 连接关闭：退出分组、广播离线、回收槽位（调用方负责 close(fd)） */
void gw_on_close(int fd);

/* 下行数据统一入口：任意线程可调（CAN 侧在 Reactor 线程，Modbus 在轮询线程）。
 * source 形如 "can:0x100" / "modbus:1" / "tcp:dev1"，分组取 ':' 前的部分 */
void gw_dispatch_frame(const char* source, const char* key, const char* value);

/* 网关累计统计：收进的下行帧数 / 分发出去的条数 */
void gw_get_stat(unsigned long long* frames_in, unsigned long long* frames_out);

#endif /* GW_GATEWAY_H */
