/*
 * gwcache_mon —— /dev/gwcache 监控端
 *
 *   ./GwcacheMon        poll + read 跟踪实时快照流（可重定向到文件）
 *   ./GwcacheMon -s     打印统计（ioctl GWCACHE_GET_STAT）
 *   ./GwcacheMon -c     清空缓冲并复位计数（ioctl GWCACHE_CLEAR）
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gwcache_uapi.h"

static void usage(const char* prog) {
  fprintf(stderr,
          "用法: %s [-s|-c]\n"
          "  (无参数)  poll + read 跟踪快照流\n"
          "  -s        打印统计\n"
          "  -c        清空缓冲\n",
          prog);
}

int main(int argc, char** argv) {
  const char* mode = (argc > 1) ? argv[1] : NULL;
  int fd = open(GWCACHE_DEV, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "open %s: %s\n", GWCACHE_DEV, strerror(errno));
    fprintf(stderr, "提示: sudo insmod gwcache.ko\n");
    return 1;
  }

  if (mode && strcmp(mode, "-s") == 0) {
    struct gwcache_stat st;
    if (ioctl(fd, GWCACHE_GET_STAT, &st) < 0) {
      perror("ioctl GWCACHE_GET_STAT");
      close(fd);
      return 1;
    }
    printf("size=%u len=%u in=%llu out=%llu drop=%llu\n", st.size, st.len,
           (unsigned long long)st.in, (unsigned long long)st.out,
           (unsigned long long)st.drop);
    close(fd);
    return 0;
  }

  if (mode && strcmp(mode, "-c") == 0) {
    if (ioctl(fd, GWCACHE_CLEAR) < 0) {
      perror("ioctl GWCACHE_CLEAR");
      close(fd);
      return 1;
    }
    close(fd);
    return 0;
  }

  if (mode) {
    usage(argv[0]);
    close(fd);
    return 2;
  }

  char buf[4096];
  struct pollfd pfd = {.fd = fd, .events = POLLIN};

  for (;;) {
    int r = poll(&pfd, 1, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }
    if (!(pfd.revents & POLLIN))
      continue;

    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      perror("read");
      break;
    }
    if (n == 0)
      continue;
    if (write(STDOUT_FILENO, buf, (size_t)n) < 0)
      break;
  }

  close(fd);
  return 1;
}
