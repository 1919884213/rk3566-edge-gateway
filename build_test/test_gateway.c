/*
 * 网关核心功能自检：把临时文件当 socket，直接驱动 gw_on_bytes，
 * 覆盖 注册 -> 订阅 -> 设备上报 -> 分组分发 -> 退订 -> 离线 全链路，
 * 并单独校验 CAN / Modbus 两条下行是否走同一条分发出口。
 *
 * 不依赖 broker 与内核驱动：MQTT 未启动时 gwup_enqueue 直接返回，
 * /dev/gwcache 打不开时 gwcache_emit 是空操作 —— 降级路径顺便一起验了。
 *
 * 断言失败即非 0 退出，可直接进 CI。
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "Gateway.h"
#include "main.h"

static int g_fail = 0;

static void check(int cond, const char* what) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond)
    g_fail++;
}

static int fake_sock(const char* path) {
  unlink(path);
  return open(path, O_RDWR | O_CREAT, 0644);
}

static void feed(int fd, const char* s) {
  (void)gw_on_bytes(fd, s, strlen(s));
}

static const char* dump(const char* path, char* buf, size_t sz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    buf[0] = '\0';
    return buf;
  }
  ssize_t n = read(fd, buf, sz - 1);
  close(fd);
  buf[n > 0 ? n : 0] = '\0';
  return buf;
}

static int has(const char* hay, const char* needle) {
  return strstr(hay, needle) != NULL;
}

int main(void) {
  char outA[8192], outB[8192];

  int A = fake_sock("gw_A.tmp");
  int B = fake_sock("gw_B.tmp");
  printf("fake fds: A=%d B=%d\n", A, B);

  gw_init(MAX_CONN);
  gw_fd_assign(A, 0);
  gw_fd_assign(B, 1);
  gw_on_accept(A);
  gw_on_accept(B);

  printf("--- 设备注册（含 ID 查重）---\n");
  feed(A, "plc_01\n");
  feed(B, "plc_01\n");      /* 重名应被拒 */
  feed(B, "plc_02\n");
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "已被占用"), "重名设备 ID 被拒绝");

  printf("--- 分组订阅 ---\n");
  feed(A, "SUB tcp\n");
  feed(B, "SUB tcp\n");
  dump("gw_A.tmp", outA, sizeof(outA));
  check(has(outA, "已订阅分组 [tcp]"), "订阅确认回执");

  printf("--- 设备上报：走 TCP 上行 -> 分组分发 ---\n");
  memset(outA, 0, sizeof(outA));
  feed(A, "DATA temp 26\n");     /* A 上报，应分发给同组的 B */
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "DATA tcp:plc_01 temp 26"), "A 的上报以 tcp:<设备ID> 为源分发到 B");

  printf("--- 下行源收敛：CAN / Modbus 走同一条出口 ---\n");
  memset(outB, 0, sizeof(outB));
  feed(B, "SUB can\n");           /* B 改订 can 分组 */
  gw_dispatch_frame("can:0x100", "speed", "1122334455667788");
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "DATA can:0x100 speed 1122334455667788"), "CAN 帧按 can 分组分发");

  memset(outB, 0, sizeof(outB));
  feed(B, "SUB modbus\n");
  gw_dispatch_frame("modbus:1", "flow_rate", "1234");
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "DATA modbus:1 flow_rate 1234"), "Modbus 帧按 modbus 分组分发");

  printf("--- 退订后不再收到 ---\n");
  feed(B, "UNSUB\n");
  memset(outB, 0, sizeof(outB));
  gw_dispatch_frame("can:0x101", "rpm", "00FF");
  dump("gw_B.tmp", outB, sizeof(outB));
  check(!has(outB, "can:0x101"), "退订后不再收到该分组数据");

  printf("--- SUB * 订阅全部 ---\n");
  feed(B, "SUB *\n");
  memset(outB, 0, sizeof(outB));
  gw_dispatch_frame("can:0x101", "rpm", "00FF");
  gw_dispatch_frame("modbus:2", "tank_level", "77");
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "can:0x101") && has(outB, "modbus:2"), "SUB * 收到跨分组数据");

  printf("--- 统计 ---\n");
  unsigned long long fin = 0, fout = 0;
  gw_get_stat(&fin, &fout);
  printf("  下行帧 %llu / 分发 %llu\n", fin, fout);
  check(fin >= 5 && fout >= 5, "统计计数在增长");

  printf("--- 设备离线广播 ---\n");
  feed(A, "SUB tcp\n");
  memset(outB, 0, sizeof(outB));
  gw_on_close(A);
  dump("gw_B.tmp", outB, sizeof(outB));
  check(has(outB, "plc_01 已离线"), "同分组收到离线通知");

  gw_on_close(B);
  printf("\n===== 结果: %s（失败 %d 项）=====\n", g_fail ? "FAILED" : "ALL PASS",
         g_fail);
  return g_fail ? 1 : 0;
}
