#ifndef GW_MQTT_H
#define GW_MQTT_H

#include <stddef.h>
#include <stdint.h>

/*
 * 上行 MQTT —— 自实现的最小 MQTT 3.1.1 客户端（保住「服务端核心零第三方依赖」）。
 *
 * 只实现网关真正需要的四种报文：CONNECT / PUBLISH(QoS0) / PINGREQ / DISCONNECT。
 * 明确不做的：TLS、QoS1/2、SUBSCRIBE、遗嘱。要 TLS 就换成 mosquitto 客户端，
 * 那会把「零依赖」这条卖点一起换掉，所以这里是权衡后的取舍，不是遗漏。
 */

typedef struct GwMqtt GwMqtt;

/* MQTT 剩余长度变长整数编码：每字节低 7 位有效，最高位是续接标志。
 * 提到头里同样是为了让纯逻辑自检能脱离 socket 头文件单独验证边界值。 */
static inline size_t gwmqtt_put_remlen(uint8_t* p, size_t len) {
  size_t i = 0;
  do {
    uint8_t b = (uint8_t)(len % 128);
    len /= 128;
    if (len > 0)
      b |= 0x80;
    p[i++] = b;
  } while (len > 0);
  return i;
}

/* 建连并完成 CONNECT/CONNACK 握手。user/pass 传 NULL 表示匿名。失败返回 NULL */
GwMqtt* gwmqtt_connect(const char* host, int port, const char* client_id,
                       const char* user, const char* pass, int keepalive_s);

/* 发布一条 QoS0 报文（收即忘，不等 PUBACK）。返回 0 成功，-1 失败 */
int  gwmqtt_publish(GwMqtt* m, const char* topic, const char* payload);

int  gwmqtt_ping(GwMqtt* m);
void gwmqtt_disconnect(GwMqtt* m);

/* ============================ 上行线程 ============================
 * 云侧的网络时延/断连不能拖住 Reactor：数据帧先塞进无锁外的环形队列，
 * 由独立线程慢慢发，发不动就丢弃并计数（和旁路缓存同一个原则：
 * 上行通道不能因为自己堵了就让采集/转发路径跟着卡）。
 */
int  gwup_start(const char* host, int port, const char* client_id,
                const char* user, const char* pass, const char* topic_prefix);

/* 入队一帧（非阻塞，队列满则丢最旧并计数）。可从任意线程调用 */
void gwup_enqueue(const char* source, const char* key, const char* value);

void gwup_stop(void);

/* 读统计：返回已发布条数，*dropped 写入丢弃条数 */
unsigned long long gwup_stats(unsigned long long* dropped);

#endif /* GW_MQTT_H */
