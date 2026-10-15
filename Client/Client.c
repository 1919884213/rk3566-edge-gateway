/*
 * gwclient —— 现场设备模拟器 / 订阅查看器
 *
 *   ./gwclient                     交互式：注册设备后手敲 SUB / DATA / 组内消息
 *   ./gwclient sim <N> [host] [port]   模拟 N 个 TCP 设备，各自注册并周期上报
 *   ./gwclient watch <分组> [host] [port]  订阅某分组，持续打印收到的数据帧
 *
 * 单线程 select 同时监听 stdin 与 socket —— 边收数据边敲命令，不用多线程。
 */
#include "Client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "main.h"

static int client_connect(const char* host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    fprintf(stderr, "invalid address: %s\n", host);
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

static void send_line(int fd, const char* s) {
  if (write(fd, s, strlen(s)) < 0 || write(fd, "\n", 1) < 0)
    perror("write");
}

/* 模拟模式：N 个设备各自注册、订阅 tcp 分组，并周期上报一条遥测 */
static int run_sim(int n, const char* host, int port) {
  int fd[64];
  if (n > 64)
    n = 64;
  printf("[sim] 启动 %d 个模拟设备 -> %s:%d\n", n, host, port);
  for (int i = 0; i < n; i++) {
    fd[i] = client_connect(host, port);
    if (fd[i] < 0)
      return 1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "dev_%02d", i);
    send_line(fd[i], cmd);          /* 第一行 = 注册设备 ID */
    send_line(fd[i], "SUB tcp");
  }

  for (int round = 0; round < 10; round++) {   /* 每设备上报 10 条后退出 */
    for (int i = 0; i < n; i++) {
      char cmd[128];
      snprintf(cmd, sizeof(cmd), "DATA temp %d", 20 + ((round + i) % 15));
      send_line(fd[i], cmd);
    }
    usleep(500 * 1000);
  }

  for (int i = 0; i < n; i++) {
    send_line(fd[i], "QUIT");
    close(fd[i]);
  }
  printf("[sim] 完成\n");
  return 0;
}

/* 订阅查看模式：只收不敲，用来验收「网关确实在往下发」 */
static int run_watch(const char* group, const char* host, int port) {
  int fd = client_connect(host, port);
  if (fd < 0)
    return 1;
  send_line(fd, "viewer");
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "SUB %s", group);
  send_line(fd, cmd);
  printf("[watch] 订阅 %s，Ctrl+C 退出\n", group);

  char buf[BUF_SIZE];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
      break;
    buf[n] = '\0';
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
  }
  close(fd);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 2 && strcmp(argv[1], "sim") == 0)
    return run_sim(atoi(argv[2]), argc > 3 ? argv[3] : SERVERADDR,
                   argc > 4 ? atoi(argv[4]) : PORT);
  if (argc > 2 && strcmp(argv[1], "watch") == 0)
    return run_watch(argv[2], argc > 3 ? argv[3] : SERVERADDR,
                     argc > 4 ? atoi(argv[4]) : PORT);

  const char* host = argc > 1 ? argv[1] : SERVERADDR;
  int port = argc > 2 ? atoi(argv[2]) : PORT;
  int fd = client_connect(host, port);
  if (fd < 0)
    return 1;
  printf("[gwclient] 已连接 %s:%d，先输入设备 ID 注册 (QUIT 退出)\n", host, port);

  char buf[BUF_SIZE];
  while (1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(fd, &rfds);

    int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
    int r = select(maxfd, &rfds, NULL, NULL, NULL);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    if (FD_ISSET(fd, &rfds)) {
      ssize_t n = read(fd, buf, sizeof(buf) - 1);
      if (n <= 0) {
        printf("\n[gwclient] 网关闭了连接\n");
        break;
      }
      buf[n] = '\0';
      fwrite(buf, 1, (size_t)n, stdout);
      fflush(stdout);
    }

    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      if (!fgets(buf, sizeof(buf), stdin))
        break;
      size_t len = strlen(buf);
      if (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
      if (write(fd, buf, len) < 0 || write(fd, "\n", 1) < 0) {
        perror("write");
        break;
      }
      if (strcasecmp(buf, "QUIT") == 0 || strcasecmp(buf, "/exit") == 0)
        break;
    }
  }

  close(fd);
  return 0;
}
