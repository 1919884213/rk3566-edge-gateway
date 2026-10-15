#ifndef MAIN_H
#define MAIN_H

/* ============================ 网络接入 ============================ */
#define PORT        8080
#define MAX_NUM     1024
#define SERVERADDR  "127.0.0.1"

/* 工作线程数量 */
#define WORKER_NUM  4
/* 连接表容量，同时也是可管理的最大 fd */
#define MAX_CONN    1024
/* 单连接读写缓冲区大小 */
#define BUF_SIZE    4096

/* ======================= 下行一：SocketCAN ======================= */
/* CAN 接口名；设为空串则关闭 CAN 通道（板子没接 CAN 收发器时的降级路径） */
#define GW_CAN_IF   "can0"

/* ===================== 下行二：Modbus RTU ======================= */
#define GW_MB_DEV       "/dev/ttyS3"   /* 串口设备；空串 = 关闭 Modbus 通道 */
#define GW_MB_BAUD      9600
#define GW_MB_PARITY    'N'
#define GW_MB_PERIOD_MS 1000           /* 轮询周期 */

/* ======================= 上行：MQTT 上报 =========================
 * 下面是编译期默认值；运行时可用环境变量覆盖（见 main.c）：
 *   GW_MQTT_HOST / GW_MQTT_PORT / GW_MQTT_TOPIC / GW_MQTT_USER / GW_MQTT_PASS
 * 未配置 broker（host 为空）时整条上行通道关闭，网关照常采集与本地分发。
 */
#define GW_MQTT_HOST    ""
#define GW_MQTT_PORT    1883
#define GW_MQTT_CLIENT  "rk3566-gateway"
#define GW_MQTT_USER    ""
#define GW_MQTT_PASS    ""
#define GW_MQTT_TOPIC   "gateway/data"

#endif
