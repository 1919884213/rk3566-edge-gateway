#ifndef SERVER_H
#define SERVER_H

#include "arpa/inet.h"
#include "pool.h"
#include "sys/socket.h"

/* 初始化 TCP 服务器：socket -> setsockopt -> bind -> listen，返回监听 fd */
int Server_init(void);

/*
 * 启动 Reactor 事件循环。
 *
 * epoll 实例里同时纳管两类异构事件源：
 *   1) socket：listen fd（水平触发，循环 accept 到 EAGAIN）+ 客户端 fd（ET+ONESHOT）
 *   2) SocketCAN：can fd（ET）—— CAN socket 天然可被 poll/epoll 纳管，
 *      所以下行 CAN 不走采集线程，而是作为普通事件源复用同一个 Reactor
 * canfd 传 -1 表示板子没有 CAN 接口，网关降级为「仅 TCP + Modbus」继续运行。
 */
void server_run(int serverfd, Threadpool* pool, int canfd);

#endif
