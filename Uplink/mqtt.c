/* Uplink/mqtt.c —— 最小 MQTT 3.1.1 客户端 + 上行发送线程 */
#define _GNU_SOURCE
#include "mqtt.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "frame.h"

#define GWUP_QUEUE     512              /* 环形队列槽数 */
#define GWUP_TOPIC_MAX 96
#define GWUP_PAY_MAX   96
#define GWUP_RCV_MS    3000
#define GWUP_SND_MS    3000

struct GwMqtt {
  int fd;
  int keepalive_s;
  time_t last_tx;   /* 上次发报文时刻，用于决定何时发 PINGREQ */
};

/* ============================ 报文编码 ============================ */

static size_t put_str(uint8_t* p, const char* s) {
  size_t n = strlen(s);
  p[0] = (uint8_t)(n >> 8);
  p[1] = (uint8_t)(n & 0xFF);
  memcpy(p + 2, s, n);
  return n + 2;
}

static int write_all(int fd, const uint8_t* buf, size_t len) {
  while (len > 0) {
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n > 0) {
      buf += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return -1;
  }
  return 0;
}

/* 按精确长度读，MQTT 控制报文是定长的，短读要补齐 */
static int read_exact(int fd, uint8_t* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, buf + got, len - got, 0);
    if (n > 0) {
      got += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return -1;
  }
  return 0;
}

/* ============================ 连接 ============================ */

static int tcp_connect(const char* host, int port) {
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
    return -1;

  int fd = -1;
  for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0)
    return -1;

  struct timeval tv = {.tv_sec = GWUP_RCV_MS / 1000,
                       .tv_usec = (GWUP_RCV_MS % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  tv.tv_sec = GWUP_SND_MS / 1000;
  tv.tv_usec = (GWUP_SND_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

GwMqtt* gwmqtt_connect(const char* host, int port, const char* client_id,
                       const char* user, const char* pass, int keepalive_s) {
  int fd = tcp_connect(host, port);
  if (fd < 0)
    return NULL;

  uint8_t pkt[512];
  uint8_t body[384];
  size_t bl = 0;

  bl += put_str(body + bl, "MQTT");   /* 协议名 */
  body[bl++] = 4;                     /* 协议级别 3.1.1 */
  uint8_t flags = 0x02;               /* Clean Session */
  if (user) flags |= 0x80;
  if (pass) flags |= 0x40;
  body[bl++] = flags;
  body[bl++] = (uint8_t)(keepalive_s >> 8);
  body[bl++] = (uint8_t)(keepalive_s & 0xFF);
  bl += put_str(body + bl, client_id);
  if (user) bl += put_str(body + bl, user);
  if (pass) bl += put_str(body + bl, pass);

  size_t hl = 1 + gwmqtt_put_remlen(pkt + 1, bl);
  pkt[0] = 0x10;                      /* CONNECT */
  memcpy(pkt + hl, body, bl);

  if (write_all(fd, pkt, hl + bl) < 0) {
    close(fd);
    return NULL;
  }

  uint8_t ack[4];
  if (read_exact(fd, ack, 4) < 0 || ack[0] != 0x20 || ack[3] != 0x00) {
    close(fd);                        /* ack[3] 是返回码，非 0 即拒绝 */
    return NULL;
  }

  GwMqtt* m = (GwMqtt*)calloc(1, sizeof(GwMqtt));
  if (m == NULL) {
    close(fd);
    return NULL;
  }
  m->fd = fd;
  m->keepalive_s = keepalive_s > 0 ? keepalive_s : 60;
  m->last_tx = time(NULL);
  return m;
}

int gwmqtt_publish(GwMqtt* m, const char* topic, const char* payload) {
  if (m == NULL || m->fd < 0)
    return -1;
  uint8_t pkt[512];
  uint8_t body[384];
  size_t bl = 0;
  bl += put_str(body + bl, topic);
  size_t pl = strlen(payload);
  if (bl + pl > sizeof(body))
    return -1;
  memcpy(body + bl, payload, pl);
  bl += pl;

  size_t hl = 1 + gwmqtt_put_remlen(pkt + 1, bl);
  pkt[0] = 0x30;                      /* PUBLISH, QoS0, 不保留 */
  memcpy(pkt + hl, body, bl);

  if (write_all(m->fd, pkt, hl + bl) < 0)
    return -1;
  m->last_tx = time(NULL);
  return 0;
}

int gwmqtt_ping(GwMqtt* m) {
  if (m == NULL || m->fd < 0)
    return -1;
  uint8_t pkt[2] = {0xC0, 0x00};      /* PINGREQ */
  if (write_all(m->fd, pkt, 2) < 0)
    return -1;
  m->last_tx = time(NULL);
  return 0;
}

void gwmqtt_disconnect(GwMqtt* m) {
  if (m == NULL)
    return;
  if (m->fd >= 0) {
    uint8_t pkt[2] = {0xE0, 0x00};    /* DISCONNECT */
    (void)write_all(m->fd, pkt, 2);
    close(m->fd);
  }
  free(m);
}

/* ============================ 上行线程 ============================ */

typedef struct {
  char source[GW_SRC_MAX];
  char key[GW_KEY_MAX];
  char value[GW_VAL_MAX];
} UpSlot;

static struct {
  UpSlot            ring[GWUP_QUEUE];
  int               head, tail, count;
  pthread_mutex_t   mtx;
  pthread_cond_t    cv;
  pthread_t         tid;
  int               running;
  char              host[64], client_id[48], user[48], pass[48];
  char              topic_prefix[GWUP_TOPIC_MAX];
  int               port;
  unsigned long long published, dropped, reconnects;
} g_up = {.mtx = PTHREAD_MUTEX_INITIALIZER,
          .cv = PTHREAD_COND_INITIALIZER,
          .running = 0};

void gwup_enqueue(const char* source, const char* key, const char* value) {
  if (!g_up.running)
    return;
  pthread_mutex_lock(&g_up.mtx);
  if (g_up.count == GWUP_QUEUE) {
    /* 队列满：丢最旧的那条并计数。上行是旁路，不能反压采集路径 */
    g_up.head = (g_up.head + 1) % GWUP_QUEUE;
    g_up.count--;
    g_up.dropped++;
  }
  UpSlot* s = &g_up.ring[g_up.tail];
  snprintf(s->source, sizeof(s->source), "%s", source);
  snprintf(s->key, sizeof(s->key), "%s", key);
  snprintf(s->value, sizeof(s->value), "%s", value);
  g_up.tail = (g_up.tail + 1) % GWUP_QUEUE;
  g_up.count++;
  pthread_cond_signal(&g_up.cv);
  pthread_mutex_unlock(&g_up.mtx);
}

static void* up_loop(void* arg) {
  (void)arg;
  GwMqtt* m = NULL;
  int backoff_ms = 1000;

  while (g_up.running) {
    if (m == NULL) {
      m = gwmqtt_connect(g_up.host, g_up.port, g_up.client_id,
                         g_up.user[0] ? g_up.user : NULL,
                         g_up.pass[0] ? g_up.pass : NULL, 60);
      if (m == NULL) {
        fprintf(stderr, "[WARN] MQTT 连接 %s:%d 失败，%d ms 后重试\n",
                g_up.host, g_up.port, backoff_ms);
        for (int w = 0; w < backoff_ms && g_up.running; w += 50)
          usleep(50 * 1000);
        backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
        continue;
      }
      backoff_ms = 1000;
      g_up.reconnects++;
      printf("[INFO] MQTT 上行已连接 %s:%d\n", g_up.host, g_up.port);
    }

    UpSlot s;
    int have = 0;
    pthread_mutex_lock(&g_up.mtx);
    if (g_up.count == 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 1;                     /* 等 1s：顺便当保活心跳的节拍 */
      pthread_cond_timedwait(&g_up.cv, &g_up.mtx, &ts);
    }
    if (g_up.count > 0) {
      s = g_up.ring[g_up.head];
      g_up.head = (g_up.head + 1) % GWUP_QUEUE;
      g_up.count--;
      have = 1;
    }
    pthread_mutex_unlock(&g_up.mtx);
    if (!g_up.running)
      break;

    if (have) {
      char topic[GWUP_TOPIC_MAX + GW_SRC_MAX];
      char payload[GWUP_PAY_MAX + GW_VAL_MAX];
      snprintf(topic, sizeof(topic), "%s/%s", g_up.topic_prefix, s.source);
      snprintf(payload, sizeof(payload), "%s=%s", s.key, s.value);
      if (gwmqtt_publish(m, topic, payload) == 0) {
        g_up.published++;
      } else {
        /* 发送失败：先把它塞回队首不现实（环形队列），直接丢弃并计数，
         * 由现场数据下一周期自然补齐 —— 遥测是周期量，不需要重传队列 */
        pthread_mutex_lock(&g_up.mtx);
        g_up.dropped++;
        pthread_mutex_unlock(&g_up.mtx);
        fprintf(stderr, "[WARN] MQTT 发送失败，重连\n");
        gwmqtt_disconnect(m);
        m = NULL;
      }
    } else if (time(NULL) - m->last_tx >= m->keepalive_s / 2) {
      if (gwmqtt_ping(m) < 0) {          /* 保活探测，顺便探活连接 */
        gwmqtt_disconnect(m);
        m = NULL;
      }
    }
  }

  if (m)
    gwmqtt_disconnect(m);
  return NULL;
}

int gwup_start(const char* host, int port, const char* client_id,
               const char* user, const char* pass, const char* topic_prefix) {
  if (host == NULL || host[0] == '\0')
    return -1;                            /* 没配 broker：整个上行通道关闭 */
  memset(&g_up.ring, 0, sizeof(g_up.ring));
  g_up.head = g_up.tail = g_up.count = 0;
  g_up.published = g_up.dropped = g_up.reconnects = 0;
  snprintf(g_up.host, sizeof(g_up.host), "%s", host);
  snprintf(g_up.client_id, sizeof(g_up.client_id), "%s",
           client_id && client_id[0] ? client_id : "rk3566-gateway");
  snprintf(g_up.user, sizeof(g_up.user), "%s", user ? user : "");
  snprintf(g_up.pass, sizeof(g_up.pass), "%s", pass ? pass : "");
  snprintf(g_up.topic_prefix, sizeof(g_up.topic_prefix), "%s",
           topic_prefix && topic_prefix[0] ? topic_prefix : "gateway/data");
  g_up.port = port;
  g_up.running = 1;

  if (pthread_create(&g_up.tid, NULL, up_loop, NULL) != 0) {
    g_up.running = 0;
    return -1;
  }
  return 0;
}

void gwup_stop(void) {
  if (!g_up.running)
    return;
  g_up.running = 0;
  pthread_mutex_lock(&g_up.mtx);
  pthread_cond_broadcast(&g_up.cv);
  pthread_mutex_unlock(&g_up.mtx);
  pthread_join(g_up.tid, NULL);
}

unsigned long long gwup_stats(unsigned long long* dropped) {
  pthread_mutex_lock(&g_up.mtx);
  unsigned long long pub = g_up.published, dr = g_up.dropped;
  pthread_mutex_unlock(&g_up.mtx);
  if (dropped)
    *dropped = dr;
  return pub;
}
