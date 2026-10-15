/* Downlink/can.c —— SocketCAN 下行采集（Reactor 的第二个事件源） */
#define _GNU_SOURCE
#include "can.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frame.h"

/*
 * CAN ID -> 信号名 映射表。
 * 生产环境的这张表由 DBC 文件生成；这里内置一小张演示表。
 * 未命中的 ID 一律以 key=raw 上报 —— 不静默丢帧，比"猜出个信号名"诚实。
 */
static const struct {
  unsigned    id;
  const char* key;
} g_idmap[] = {
    {0x100, "speed"},
    {0x101, "rpm"},
    {0x102, "coolant_temp"},
    {0x180, "battery_v"},
    {0x200, "door_state"},
};

static const char* id_to_key(unsigned id) {
  for (size_t i = 0; i < sizeof(g_idmap) / sizeof(g_idmap[0]); i++)
    if (g_idmap[i].id == id)
      return g_idmap[i].key;
  return "raw";
}

int gwcan_open(const char* ifname) {
  int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd < 0)
    return -1;

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_can addr;
  memset(&addr, 0, sizeof(addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  int fl = fcntl(fd, F_GETFL);
  if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int gwcan_read_frame(int fd, char* source, size_t ssz, char* key, size_t ksz,
                      char* value, size_t vsz) {
  struct can_frame cf;
  ssize_t n = read(fd, &cf, sizeof(cf));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    if (errno == EINTR)
      return 0;
    return -1;
  }
  if ((size_t)n < sizeof(cf))
    return 0; /* 短读：忽略 */

  /* 错误帧 / 远程帧不进数据通道（错误帧应当走内核的错误计数，不是上报值） */
  if (cf.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG))
    return 0;

  unsigned id = cf.can_id & (cf.can_id & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK);

  char hex[2 * 8 + 1];
  size_t dn = cf.can_dlc > 8 ? 8 : cf.can_dlc;
  for (size_t i = 0; i < dn; i++)
    snprintf(hex + 2 * i, 3, "%02X", cf.data[i]);
  hex[2 * dn] = '\0';

  snprintf(source, ssz, "can:0x%03X", id);
  snprintf(key, ksz, "%s", id_to_key(id));
  snprintf(value, vsz, "%s", hex);
  return 1;
}

void gwcan_close(int fd) {
  if (fd >= 0)
    close(fd);
}
