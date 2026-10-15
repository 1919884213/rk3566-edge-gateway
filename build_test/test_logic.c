/*
 * 纯逻辑自检：Modbus CRC16 + MQTT 剩余长度变长整数编码。
 *
 * 这两段位运算最容易写错、又最难从外部观察（CRC 错 = 从站永远不回帧，
 * 变长整数错 = broker 直接掐连接），所以单独抽出来做单元自检。
 *
 * 只依赖标准头，不依赖 termios / socket / pthread —— 可以在任意平台原生跑：
 *   gcc -I. -IDownlink -IUplink build_test/test_logic.c -o logic_test && ./logic_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modbus.h"
#include "mqtt.h"

static int g_fail = 0;

static void check(int cond, const char* what) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond)
    g_fail++;
}

static void test_crc16(void) {
  printf("--- Modbus CRC16 ---\n");
  /* CRC-16/MODBUS 标准检查值：字符串 "123456789" -> 0x4B37 */
  check(gwmb_crc16((const uint8_t*)"123456789", 9) == 0x4B37,
        "标准检查值 \"123456789\" == 0x4B37");

  /* FC03 读从站 1、寄存器 0x0000、1 个寄存器。
   * 该请求的完整帧在公开资料里是 01 03 00 00 00 01 84 0A —— 末两字节即 CRC，
   * 顺序是「低字节先发」，所以 crc 值本身是 0x0A84。 */
  const uint8_t req[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  uint16_t crc = gwmb_crc16(req, 6);
  check(crc == 0x0A84, "FC03 请求帧 CRC == 0x0A84");
  check((crc & 0xFF) == 0x84 && (crc >> 8) == 0x0A,
        "线上顺序低字节在前 => 84 0A（与公开参考帧一致）");

  /* 单字节翻转必须改变 CRC（防"返回常数"式假实现） */
  uint8_t bad[6];
  memcpy(bad, req, 6);
  bad[5] ^= 0x01;
  check(gwmb_crc16(bad, 6) != crc, "任一字节翻转后 CRC 改变");
}

static void test_remlen(void) {
  printf("--- MQTT 剩余长度变长整数 ---\n");
  uint8_t b[4];
  size_t n;

  n = gwmqtt_put_remlen(b, 0);
  check(n == 1 && b[0] == 0x00, "0 -> 1 字节 [00]");

  n = gwmqtt_put_remlen(b, 127);
  check(n == 1 && b[0] == 0x7F, "127 -> 1 字节 [7F]（单字节上限）");

  n = gwmqtt_put_remlen(b, 128);
  check(n == 2 && b[0] == 0x80 && b[1] == 0x01, "128 -> 2 字节 [80 01]（进位点）");

  n = gwmqtt_put_remlen(b, 16383);
  check(n == 2 && b[0] == 0xFF && b[1] == 0x7F, "16383 -> 2 字节 [FF 7F]（两字节上限）");

  n = gwmqtt_put_remlen(b, 16384);
  check(n == 3 && b[0] == 0x80 && b[1] == 0x80 && b[2] == 0x01,
        "16384 -> 3 字节 [80 80 01]（继续进位）");

  n = gwmqtt_put_remlen(b, 2097151);
  check(n == 3 && b[0] == 0xFF && b[1] == 0xFF && b[2] == 0x7F,
        "2097151 -> 3 字节 [FF FF 7F]（三字节上限）");

  n = gwmqtt_put_remlen(b, 2097152);
  check(n == 4 && b[0] == 0x80 && b[1] == 0x80 && b[2] == 0x80 && b[3] == 0x01,
        "2097152 -> 4 字节（MQTT 规范的最大长度边界）");
}

int main(void) {
  test_crc16();
  test_remlen();
  printf("\n===== 结果: %s（失败 %d 项）=====\n", g_fail ? "FAILED" : "ALL PASS",
         g_fail);
  return g_fail ? 1 : 0;
}
