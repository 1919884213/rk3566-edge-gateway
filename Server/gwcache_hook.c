#include "gwcache_hook.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>   /* strerror */
#include <unistd.h>

#define GW_CACHE_DEV "/dev/gwcache"

static int g_fd = -1;

void gwcache_attach(void) {
  /* O_NONBLOCK：驱动侧本来也不阻塞，这里再加一道，保证任何情况下不拖慢分发路径 */
  g_fd = open(GW_CACHE_DEV, O_WRONLY | O_NONBLOCK);
  if (g_fd < 0)
    fprintf(stderr, "[WARN] %s 不可用(%s)，内核缓存通道关闭，网关继续运行\n",
            GW_CACHE_DEV, strerror(errno));
  else
    printf("[INFO] 内核缓存通道 -> %s\n", GW_CACHE_DEV);
}

void gwcache_emit(const char* data, size_t len) {
  if (g_fd < 0 || len == 0)
    return;
  /* 驱动缓冲满是"整条丢弃 + 计数"，业务层不重试、不报错 —— 但返回值要接住，
   * 否则碰到 glibc 的 warn_unused_result 会报警告（(void) 强制转换压不住） */
  ssize_t n = write(g_fd, data, len);
  (void)n;
}

void gwcache_detach(void) {
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
}

int gwcache_available(void) {
  return g_fd >= 0;
}
