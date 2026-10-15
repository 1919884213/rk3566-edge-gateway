#include "Server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "Gateway.h"
#include "can.h"
#include "main.h"

/* 事件源类型：同一个 epoll 实例、同一套 worker 池，处理函数按类型分派 */
typedef enum { SRC_TCP = 0, SRC_CAN } SrcKind;

/* 每个事件源的处理上下文，作为任务参数传给 worker */
typedef struct Conn {
  int     fd;
  int     epfd;
  int     worker_index;
  SrcKind kind;
} Conn;

/* 全局 epoll 实例，供 worker 重新武装 fd 时使用 */
static int g_epfd = -1;

static void die(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

/* 将 fd 设为非阻塞 */
static void set_nonblock(int fd) {
  int opt = fcntl(fd, F_GETFL);
  if (opt < 0)
    die("fcntl F_GETFL");
  if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0)
    die("fcntl F_SETFL");
}

/* 关闭连接：网关状态清理、从 epoll 摘除、关闭 fd、回收归属表项 */
static void close_conn(Conn* c) {
  gw_on_close(c->fd);
  if (c->kind == SRC_TCP) {
    epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    gw_fd_release(c->fd);
    printf("[INFO] device fd %d disconnected\n", c->fd);
  } else {
    gwcan_close(c->fd);   /* CAN 是常驻事件源，只有致命错误才走到这里 */
  }
  free(c);
}

/* ---- TCP 设备连接：边沿触发需读到 EAGAIN，把原始字节喂给网关状态机 ---- */
static void handle_tcp(Conn* c) {
  char buf[BUF_SIZE];
  int close_req = 0;

  while (!close_req) {
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n > 0) {
      int r = gw_on_bytes(c->fd, buf, (size_t)n);
      if (r == 1)
        close_req = 1; /* 设备主动 QUIT / 断开 */
      /* r == -1：fd 未登记（过期事件），继续读干净即可 */
    } else if (n == 0) {
      close_conn(c); /* 对端关闭 */
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break; /* 数据已读干净 */
      if (errno == EINTR)
        continue;
      close_conn(c);
      return;
    }
  }

  if (close_req) {
    close_conn(c);
    return;
  }

  /* 重新武装，等待下一次可读事件 */
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
  ev.data.fd = c->fd;
  if (epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
    if (errno != EEXIST)
      close_conn(c);
  }
}

/* ---- CAN 事件源：读干净所有待处理帧，逐帧走网关统一下行出口 ----
 * 这里不用 ONESHOT：CAN fd 恒定绑在同一个 worker 上，天然串行；
 * 但 ET 要求必须读到 EAGAIN，否则新帧不会再触发事件。 */
static void handle_can(Conn* c) {
  char src[GW_SRC_MAX], key[GW_KEY_MAX], val[GW_VAL_MAX];
  for (;;) {
    int n = gwcan_read_frame(c->fd, src, sizeof(src), key, sizeof(key),
                             val, sizeof(val));
    if (n > 0) {
      gw_dispatch_frame(src, key, val);   /* 与 TCP/Modbus 同一条下行出口 */
      continue;
    }
    if (n == 0)
      break;
    close_conn(c);                        /* 致命错误：接口消失/驱动卸载 */
    return;
  }
}

/* worker 任务入口：按事件源类型分派 */
static void handle_event(void* arg) {
  Conn* c = (Conn*)arg;
  if (c->kind == SRC_CAN)
    handle_can(c);
  else
    handle_tcp(c);
}

/* 初始化 TCP 服务器 */
int Server_init(void) {
  int serverfd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverfd < 0)
    die("socket");

  int opt = 1;
  if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    close(serverfd);
    die("setsockopt");
  }

  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY)};

  if (bind(serverfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(serverfd);
    die("bind");
  }
  if (listen(serverfd, MAX_NUM) < 0) {
    close(serverfd);
    die("listen");
  }
  printf("[INFO] edge gateway listening on port %d\n", PORT);
  return serverfd;
}

void server_run(int serverfd, Threadpool* pool, int canfd) {
  set_nonblock(serverfd);
  gw_init(MAX_CONN);

  g_epfd = epoll_create1(0);
  if (g_epfd < 0)
    die("epoll_create1");

  struct epoll_event ev;
  ev.data.fd = serverfd;
  ev.events = EPOLLIN;  // 监听套接字用水平触发即可
  if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, serverfd, &ev) < 0)
    die("epoll_ctl add listenfd");

  /* 异构事件源之二：CAN fd 与 socket 进同一个 epoll 实例 */
  int can_worker = -1;
  if (canfd >= 0) {
    can_worker = 0;
    gw_fd_assign(canfd, can_worker);
    struct epoll_event cev;
    cev.events = EPOLLIN | EPOLLET;
    cev.data.fd = canfd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, canfd, &cev) < 0) {
      perror("epoll_ctl add CAN fd");
      gwcan_close(canfd);
      canfd = -1;
    } else {
      printf("[INFO] CAN 事件源已接入同一 epoll 实例 (worker %d)\n", can_worker);
    }
  }

  struct epoll_event events[64];
  int next_worker = 0;  // round-robin 游标，决定下一个连接分给哪个 worker

  /*
   * Reactor 主循环：主线程只做事件分发。
   *   - 监听 fd 就绪  -> 循环 accept，把新连接绑到一个 worker 并注册进 epoll
   *   - 客户端 fd 就绪 -> 打包成任务投递给该 fd 所属 worker
   *   - CAN fd 就绪   -> 同样投递给它固定的 worker（保持帧序）
   * 真正的 read/协议解析/分发在 worker 线程里执行。
   */
  while (1) {
    int nfds = epoll_wait(g_epfd, events, 64, -1);
    if (nfds < 0) {
      if (errno == EINTR)
        continue;
      die("epoll_wait");
    }

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (fd == serverfd) {
        /* 新连接：循环 accept 到 EAGAIN，一次事件清空 accept 队列 */
        while (1) {
          struct sockaddr_in cli;
          socklen_t len = sizeof(cli);
          int cfd = accept(serverfd, (struct sockaddr*)&cli, &len);
          if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            if (errno == EINTR)
              continue;
            perror("accept");
            break;
          }
          if (cfd >= MAX_CONN) {
            fprintf(stderr, "[WARN] fd %d exceeds MAX_CONN, rejected\n", cfd);
            close(cfd);
            continue;
          }
          set_nonblock(cfd);

          int wi = next_worker;
          next_worker = (next_worker + 1) % pool->num;
          gw_fd_assign(cfd, wi);

          struct epoll_event cev;
          cev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
          cev.data.fd = cfd;
          if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
            perror("epoll_ctl add clientfd");
            close(cfd);
            gw_fd_release(cfd);
            continue;
          }
          gw_on_accept(cfd);  /* 发送欢迎语，进入设备注册状态 */
          printf("[INFO] device %s:%d connected -> worker %d\n",
                 inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), wi);
        }
        continue;
      }

      /* 已登记的事件源：投递给它绑定的 worker */
      int wi = gw_fd_get(fd);
      if (wi < 0)
        continue;  // 连接已被回收，忽略过期事件
      Conn* c = (Conn*)malloc(sizeof(Conn));
      if (c == NULL)
        continue;
      c->fd = fd;
      c->epfd = g_epfd;
      c->worker_index = wi;
      c->kind = (fd == canfd) ? SRC_CAN : SRC_TCP;
      if (pool_submit(pool, wi, handle_event, c) != 0)
        close_conn(c);
    }
  }
}
