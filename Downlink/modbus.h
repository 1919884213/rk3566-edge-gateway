#ifndef GW_MODBUS_H
#define GW_MODBUS_H

#include <stddef.h>
#include <stdint.h>

#include "frame.h"

/*
 * Modbus RTU 下行采集（串口主机）。
 *
 * 与 CAN 不同，Modbus 主机是**时间驱动**的：要按周期轮询从站寄存器，
 * 而不是等人推数据。所以它不进 epoll，而是跑一个轮询线程，
 * 采到的值仍走同一条统一帧入口 —— 事件源与轮询源在上层是收敛的。
 */

/* 一个采集点位：从站地址 + 保持寄存器地址 + 上报用的信号名 */
typedef struct {
  int         slave;
  int         reg;
  const char* key;
} GwmbPoint;

/* 打开串口并按 8N1/8E1/8O1 配置（parity: 'N'/'E'/'O'），返回非阻塞 fd；失败 -1 */
int  gwmb_open(const char* dev, int baud, char parity);
void gwmb_close(int fd);

/* Modbus CRC16（多项式 0xA001，低字节在前）。
 * 定义在头里是为了让 build_test/test_logic.c 能在不依赖 termios/poll 的
 * 平台上原生跑起来 —— 这段位运算最容易写错，必须能单独验证。 */
static inline uint16_t gwmb_crc16(const uint8_t* buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

/* 读保持寄存器 FC03：成功返回读到的寄存器个数，失败返回 -1（errno 已设置） */
int  gwmb_read_holding(int fd, int slave, int reg, int count, uint16_t* out);

/* 写单个保持寄存器 FC06：成功返回 0，失败 -1 */
int  gwmb_write_reg(int fd, int slave, int reg, uint16_t val);

/* 轮询线程主体：按 period_ms 周期采集 pts 里的点位，逐点调用 sink。
 * 单点连续失败 >=3 次打一条告警，但不退出（现场设备掉线是常态）。
 * stop 指向的变量被置非 0 时返回 0。 */
int  gwmb_poll_loop(int fd, const GwmbPoint* pts, int n, int period_ms,
                    gw_frame_sink sink, volatile int* stop);

#endif /* GW_MODBUS_H */
