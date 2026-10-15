#ifndef GWCACHE_HOOK_H
#define GWCACHE_HOOK_H

#include <stddef.h>

/*
 * 网关数据 → 内核旁路缓存通道的用户态接入。
 * 驱动没装、/dev/gwcache 不存在时全部降级为空操作，网关照常运行。
 */

/* 启动时打开设备（失败只告警，不阻止网关启动）。在 server_run 前调用 */
void gwcache_attach(void);

/* 把一条分发帧原样送进内核环形缓冲。非阻塞：驱动的缓冲满会整条丢弃并计数 */
void gwcache_emit(const char* data, size_t len);

/* 退出时关闭；也可不调用，进程结束由内核回收 */
void gwcache_detach(void);

/* 通道是否就绪（面板/STAT 用它区分「未装驱动」与「装了但没数据」） */
int  gwcache_available(void);

#endif
