/* Downlink/modbus.c —— Modbus RTU 主机（轮询式下行采集） */
#define _GNU_SOURCE
#include "modbus.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define GWMB_TIMEOUT_MS 300
#define GWMB_FAIL_ALARM 3

static speed_t baud_const(int baud) {
  switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
  }
}

int gwmb_open(const char* dev, int baud, char parity) {
  int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    return -1;

  struct termios tio;
  if (tcgetattr(fd, &tio) < 0) {
    close(fd);
    return -1;
  }
  cfmakeraw(&tio);                       /* 关掉回显/行缓冲/特殊字符 */
  tio.c_cflag |= (CLOCAL | CREAD);       /* 忽略调制解调器控制线，开启接收 */
  tio.c_cflag &= ~CSIZE;
  tio.c_cflag |= CS8;                    /* 8 数据位 */
  tio.c_cflag &= ~CSTOPB;                /* 1 停止位 */
  tio.c_cflag &= ~CRTSCTS;               /* 无硬件流控 */
  switch (parity) {
    case 'E': tio.c_cflag |= PARENB;  tio.c_cflag &= ~PARODD; break;
    case 'O': tio.c_cflag |= PARENB;  tio.c_cflag |=  PARODD; break;
    default:  tio.c_cflag &= ~PARENB; break;  /* 'N' */
  }
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  cfsetispeed(&tio, baud_const(baud));
  cfsetospeed(&tio, baud_const(baud));

  if (tcsetattr(fd, TCSANOW, &tio) < 0) {
    close(fd);
    return -1;
  }
  tcflush(fd, TCIOFLUSH);  /* 丢掉上电残留，否则第一帧必然对不上 */
  return fd;
}

void gwmb_close(int fd) {
  if (fd >= 0)
    close(fd);
}

/* 发请求 + 收响应，返回收到的字节数；超时返回 -1 */
static ssize_t mb_transact(int fd, const uint8_t* req, size_t reqlen,
                           uint8_t* resp, size_t respsz) {
  ssize_t w = write(fd, req, reqlen);
  if (w != (ssize_t)reqlen)
    return -1;

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, GWMB_TIMEOUT_MS);
  if (pr <= 0)
    return -1;  /* 超时或出错 —— 现场掉线/接线错都会走这里 */

  size_t got = 0;
  while (got < respsz) {
    ssize_t n = read(fd, resp + got, respsz - got);
    if (n > 0) {
      got += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EAGAIN) {
      /* 半帧：给驱动一点时间把剩下的字节搬上来 */
      struct pollfd p2 = {.fd = fd, .events = POLLIN};
      if (poll(&p2, 1, 20) <= 0)
        break;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  return (ssize_t)got;
}

static void put_crc(uint8_t* p, size_t len) {
  uint16_t crc = gwmb_crc16(p, len);
  p[len]     = (uint8_t)(crc & 0xFF);         /* CRC 低字节在前 */
  p[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
}

/* 校验响应帧：长度、CRC、从站号、功能码（异常响应 0x80|fc 单独判别） */
static int resp_ok(const uint8_t* resp, ssize_t n, int slave, int fc) {
  if (n < 5)
    return -1;
  if (resp[0] != (uint8_t)slave)
    return -1;
  if (resp[1] == (uint8_t)(fc | 0x80)) {
    errno = EPROTO;   /* Modbus 异常码：resp[2] 里 */
    return -1;
  }
  if (resp[1] != (uint8_t)fc)
    return -1;
  uint16_t crc = (uint16_t)(resp[n - 2] | ((uint16_t)resp[n - 1] << 8));
  if (crc != gwmb_crc16(resp, (size_t)n - 2))
    return -1;
  return 0;
}

int gwmb_read_holding(int fd, int slave, int reg, int count, uint16_t* out) {
  if (count < 1 || count > 125) {
    errno = EINVAL;
    return -1;
  }
  uint8_t req[8];
  req[0] = (uint8_t)slave;
  req[1] = 0x03;
  req[2] = (uint8_t)(reg >> 8);
  req[3] = (uint8_t)(reg & 0xFF);
  req[4] = (uint8_t)(count >> 8);
  req[5] = (uint8_t)(count & 0xFF);
  put_crc(req, 6);

  uint8_t resp[5 + 2 * 125];
  ssize_t n = mb_transact(fd, req, 8, resp, (size_t)(5 + 2 * count));
  if (n < 0 || resp_ok(resp, n, slave, 0x03) < 0)
    return -1;
  if (resp[2] != (uint8_t)(2 * count))
    return -1;

  for (int i = 0; i < count; i++)
    out[i] = (uint16_t)((resp[3 + 2 * i] << 8) | resp[4 + 2 * i]);
  return count;
}

int gwmb_write_reg(int fd, int slave, int reg, uint16_t val) {
  uint8_t req[8];
  req[0] = (uint8_t)slave;
  req[1] = 0x06;
  req[2] = (uint8_t)(reg >> 8);
  req[3] = (uint8_t)(reg & 0xFF);
  req[4] = (uint8_t)(val >> 8);
  req[5] = (uint8_t)(val & 0xFF);
  put_crc(req, 6);

  uint8_t resp[8];
  ssize_t n = mb_transact(fd, req, 8, resp, sizeof(resp));
  return resp_ok(resp, n, slave, 0x06);
}

int gwmb_poll_loop(int fd, const GwmbPoint* pts, int n, int period_ms,
                   gw_frame_sink sink, volatile int* stop) {
  int fails[64];
  if (n > (int)(sizeof(fails) / sizeof(fails[0])))
    n = (int)(sizeof(fails) / sizeof(fails[0]));
  memset(fails, 0, sizeof(fails));

  char src[GW_SRC_MAX], val[GW_VAL_MAX];
  while (stop == NULL || !*stop) {
    for (int i = 0; i < n; i++) {
      uint16_t v = 0;
      if (gwmb_read_holding(fd, pts[i].slave, pts[i].reg, 1, &v) == 1) {
        fails[i] = 0;
        snprintf(src, sizeof(src), "modbus:%d", pts[i].slave);
        snprintf(val, sizeof(val), "%u", (unsigned)v);
        sink(src, pts[i].key, val);
      } else if (++fails[i] == GWMB_FAIL_ALARM) {
        fprintf(stderr,
                "[WARN] modbus slave=%d reg=%d 连续 %d 次无响应，继续重试\n",
                pts[i].slave, pts[i].reg, fails[i]);
      }
    }
    for (int slept = 0; slept < period_ms && (stop == NULL || !*stop);
         slept += 20)
      usleep(20 * 1000);
  }
  return 0;
}
