/* main.c —— 网关装配：三条下行/上行通道 + Reactor 启动 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Gateway.h"
#include "Server.h"
#include "can.h"
#include "gwcache_hook.h"
#include "main.h"
#include "modbus.h"
#include "mqtt.h"
#include "pool.h"

/* 环境变量覆盖编译期默认值：现场改配置不用重编译 */
static const char* env_or(const char* key, const char* def) {
  const char* v = getenv(key);
  return (v && v[0]) ? v : def;
}

/* ============ Modbus 轮询线程 ============ */
typedef struct {
  int         fd;
  int         n;
  GwmbPoint   pts[8];
  volatile int* stop;
} MbTask;

static void* mb_thread(void* arg) {
  MbTask* t = (MbTask*)arg;
  gwmb_poll_loop(t->fd, t->pts, t->n, GW_MB_PERIOD_MS, gw_dispatch_frame, t->stop);
  return NULL;
}

int main(void) {
  Threadpool* pool = pool_create(WORKER_NUM);
  if (pool == NULL) {
    fprintf(stderr, "pool_create failed\n");
    return 1;
  }

  /* 旁路内核缓存通道：没装驱动/没权限就自动降级，不影响网关功能 */
  gwcache_attach();

  /* 上行 MQTT：没配 broker 直接返回 -1，整条上行关闭 */
  const char* mqtt_host = env_or("GW_MQTT_HOST", GW_MQTT_HOST);
  if (gwup_start(mqtt_host, atoi(env_or("GW_MQTT_PORT", "1883")),
                 GW_MQTT_CLIENT, env_or("GW_MQTT_USER", NULL),
                 env_or("GW_MQTT_PASS", NULL),
                 env_or("GW_MQTT_TOPIC", GW_MQTT_TOPIC)) == 0)
    printf("[INFO] MQTT 上行通道已启动 -> %s\n", mqtt_host);
  else
    printf("[INFO] 未配置 MQTT broker，上行通道关闭（本地分发照常）\n");

  /* 下行一：SocketCAN。打开成功后 fd 交给 Reactor 当事件源，不另开线程 */
  int canfd = -1;
  if (GW_CAN_IF[0]) {
    canfd = gwcan_open(GW_CAN_IF);
    if (canfd < 0)
      fprintf(stderr, "[WARN] CAN %s 打开失败(%m)，CAN 通道降级关闭\n",
              GW_CAN_IF);
    else
      printf("[INFO] CAN 通道已打开 %s\n", GW_CAN_IF);
  }

  /* 下行二：Modbus RTU。时间驱动的轮询源，跑独立线程 */
  static volatile int mb_stop = 0;
  static MbTask mbt;
  pthread_t mbtid;
  int mb_started = 0;
  if (GW_MB_DEV[0]) {
    mbt.fd = gwmb_open(GW_MB_DEV, GW_MB_BAUD, GW_MB_PARITY);
    if (mbt.fd < 0) {
      fprintf(stderr, "[WARN] Modbus 串口 %s 打开失败(%m)，Modbus 通道降级关闭\n",
              GW_MB_DEV);
    } else {
      /* 采集点位表：生产环境应由配置文件加载，这里给一张演示表 */
      mbt.pts[0] = (GwmbPoint){1, 0x0000, "flow_rate"};
      mbt.pts[1] = (GwmbPoint){1, 0x0001, "pressure"};
      mbt.pts[2] = (GwmbPoint){2, 0x0000, "tank_level"};
      mbt.n = 3;
      mbt.stop = &mb_stop;
      if (pthread_create(&mbtid, NULL, mb_thread, &mbt) == 0) {
        mb_started = 1;
        printf("[INFO] Modbus 通道已打开 %s @%d 8%c1，轮询 %d 点\n",
               GW_MB_DEV, GW_MB_BAUD, GW_MB_PARITY, mbt.n);
      }
    }
  }

  int serverfd = Server_init();
  server_run(serverfd, pool, canfd);   /* 事件循环，正常情况不会返回 */

  mb_stop = 1;
  if (mb_started)
    pthread_join(mbtid, NULL);
  if (mbt.fd >= 0)
    gwmb_close(mbt.fd);
  gwup_stop();
  gwcache_detach();
  close(serverfd);
  pool_destroy(pool);
  return 0;
}
