#ifndef GW_CAN_H
#define GW_CAN_H

#include <stddef.h>

/*
 * SocketCAN 下行采集。
 *
 * 与 Modbus 不同，CAN socket 天然可被 poll/epoll 纳管，所以它是**事件源**：
 * CAN fd 直接进主 epoll 实例，和 listen fd / client fd 用同一套 waitqueue
 * 机制复用同一个 Reactor，不需要额外的采集线程。
 */

/* 打开并绑定 CAN 接口（如 "can0"），返回设为非阻塞的 CAN socket fd；失败返回 -1 */
int  gwcan_open(const char* ifname);

/* 从 CAN fd 读一帧，拆成统一帧的三个字段（source / key / value）。
 * 返回：1 取到一帧；0 暂时无数据（或读到非数据帧，已跳过）；-1 致命错误 */
int  gwcan_read_frame(int fd, char* source, size_t ssz, char* key, size_t ksz,
                      char* value, size_t vsz);

void gwcan_close(int fd);

#endif /* GW_CAN_H */
