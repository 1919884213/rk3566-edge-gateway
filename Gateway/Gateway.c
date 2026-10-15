/* Gateway/Gateway.c —— 网关核心：设备表 + 分组订阅 + 帧路由 + 分发 */
#include "Gateway.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "gwcache_hook.h"
#include "main.h"
#include "mqtt.h"

/* ============================ 数据结构 ============================ */

#define DEVID_LEN 24
#define GROUP_LEN 24
#define LINE_MAX  1024   /* 单条上报/命令最大长度 */

typedef enum {
  ST_REG = 0,   /* 刚连上：等待设备注册 */
  ST_MENU,      /* 已注册：SUB / DATA / LIST / STAT */
  ST_GROUP      /* 已订阅分组：普通行 = 组内消息 */
} State;

typedef struct {
  int   used;                 /* 槽位是否被占用（fd 是否登记） */
  int   worker;               /* 绑定的 worker 下标，-1 空闲 */
  State st;
  char  devid[DEVID_LEN];
  int   group_idx;            /* 订阅的分组下标，-1 = 未订阅 */
  int   sub_all;              /* SUB * ：订阅全部分组 */
  char  line[LINE_MAX];       /* 半行缓存：ET 读到的字节可能不含完整行 */
  int   line_len;
  int   overflow;             /* 超长行标记：剩余部分直接丢弃 */
} Conn;

typedef struct {
  int  used;
  char name[GROUP_LEN];
} Group;

static Conn   g_conn[MAX_CONN];
static Group  g_group[MAX_CONN];
static int    g_group_cnt = 0;
static size_t g_cap = 0;

static unsigned long long g_frames_in = 0;
static unsigned long long g_frames_out = 0;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ============================ 基础工具 ============================ */

/* 非阻塞写：TCP 缓冲区满就丢弃（实时遥测允许丢最新一条，绝不允许卡线程）。
 * 持全局锁发送，重试次数必须小：8 次 ≈ 8ms 封顶，防一个慢订阅者拖死整条分发路径。 */
static void send_to(int fd, const char* data, size_t len) {
  for (int tries = 0; tries < 8 && len > 0;) {
    ssize_t n = write(fd, data, len);
    if (n > 0) {
      data += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      usleep(1000);
      tries++;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return; /* 对端已断开等错误：静默忽略，Reactor 会负责回收 */
  }
}

static void send_str(int fd, const char* s) {
  send_to(fd, s, strlen(s));
}

/* source 的分组前缀："can:0x100" -> "can" */
static const char* source_group(const char* source, char* buf, size_t sz) {
  const char* c = strchr(source, ':');
  size_t n = c ? (size_t)(c - source) : strlen(source);
  if (n >= sz)
    n = sz - 1;
  memcpy(buf, source, n);
  buf[n] = '\0';
  return buf;
}

static Group* group_find_or_add(const char* name) {
  for (int i = 0; i < g_group_cnt; i++)
    if (g_group[i].used && strcmp(g_group[i].name, name) == 0)
      return &g_group[i];
  for (int i = 0; i < g_group_cnt; i++)          /* 复用已回收的空洞槽位 */
    if (!g_group[i].used) {
      g_group[i].used = 1;
      snprintf(g_group[i].name, sizeof(g_group[i].name), "%s", name);
      return &g_group[i];
    }
  if (g_group_cnt >= MAX_CONN)
    return NULL;
  Group* g = &g_group[g_group_cnt++];
  g->used = 1;
  snprintf(g->name, sizeof(g->name), "%s", name);
  return g;
}

/* 按分组分发（调用方必须持锁）：把一条消息发给订阅该分组的连接。
 * 下行数据帧走这里，组内互发消息也走这里 —— 同一套订阅关系两种用法。 */
static void group_fanout(const char* group, const char* data, size_t len,
                         int except_fd) {
  for (size_t i = 0; i < g_cap; i++) {
    Conn* c = &g_conn[i];
    if (!c->used || c->st != ST_GROUP)
      continue;
    if ((int)i == except_fd)
      continue;
    if (!c->sub_all &&
        (c->group_idx < 0 || strcmp(g_group[c->group_idx].name, group) != 0))
      continue;
    send_to((int)i, data, len);
    g_frames_out++;
  }
}

/* ============================ 下行统一出口 ============================ */

/* 调用方必须持 g_mtx */
static void dispatch_locked(const char* source, const char* key,
                            const char* value) {
  char line[GW_LINE_MAX];
  int n = snprintf(line, sizeof(line), GW_FRAME_PREFIX "%s %s %s\n", source,
                   key, value);
  if (n <= 0 || (size_t)n >= sizeof(line))
    return;

  g_frames_in++;

  /* 出口一：内核旁路缓存（进程被 kill 后缓冲仍在，可取证） */
  gwcache_emit(line, (size_t)n);

  /* 出口二：按来源分组分发给已订阅的 TCP 连接 */
  char grp[GW_SRC_MAX];
  group_fanout(source_group(source, grp, sizeof(grp)), line, (size_t)n, -1);
}

void gw_dispatch_frame(const char* source, const char* key, const char* value) {
  if (source == NULL || key == NULL || value == NULL)
    return;
  pthread_mutex_lock(&g_mtx);
  dispatch_locked(source, key, value);
  pthread_mutex_unlock(&g_mtx);

  /* 出口三：MQTT 上行。放在锁外入队，避免与上行线程的队列锁嵌套 */
  gwup_enqueue(source, key, value);
}

/* ============================ 状态机 ============================ */

static void print_menu(int fd) {
  send_str(fd,
           "命令:\n"
           "  SUB <分组>     订阅分组数据 (can / modbus / tcp / * = 全部)\n"
           "  UNSUB          退订\n"
           "  DATA <键> <值> 上报一条数据\n"
           "  LIST           分组与订阅者\n"
           "  STAT           网关统计\n"
           "  QUIT           断开连接\n");
}

static void do_sub(Conn* c, int fd, const char* name) {
  while (*name == ' ')
    name++;
  if (name[0] == '\0') {
    send_str(fd, "[错误] 用法: SUB <分组>\n");
    return;
  }
  if (strcmp(name, "*") == 0) {
    c->sub_all = 1;
    c->group_idx = -1;
    c->st = ST_GROUP;
    send_str(fd, "已订阅全部分组\n");
    return;
  }
  Group* g = group_find_or_add(name);
  if (g == NULL) {
    send_str(fd, "[错误] 分组数量已达上限\n");
    return;
  }
  c->sub_all = 0;
  c->group_idx = (int)(g - g_group);
  c->st = ST_GROUP;

  /* 订阅时给个即时反馈，方便现场核对谁在线 */
  int cnt = 0;
  for (size_t i = 0; i < g_cap; i++)
    if (g_conn[i].used && g_conn[i].st == ST_GROUP &&
        (g_conn[i].sub_all ||
         (g_conn[i].group_idx >= 0 && g_conn[i].group_idx == c->group_idx)))
      cnt++;
  char msg[128];
  int n = snprintf(msg, sizeof(msg), "已订阅分组 [%s]，当前 %d 个订阅者\n",
                   name, cnt);
  send_to(fd, msg, (size_t)n);
}

static void do_unsub(Conn* c, int fd) {
  if (c->st != ST_GROUP) {
    send_str(fd, "[错误] 当前未订阅任何分组\n");
    return;
  }
  c->group_idx = -1;
  c->sub_all = 0;
  c->st = ST_MENU;
  send_str(fd, "已退订\n");
  print_menu(fd);
}

static void do_list(int fd) {
  send_str(fd, "分组列表:\n");
  for (int i = 0; i < g_group_cnt; i++) {
    if (!g_group[i].used)
      continue;
    int cnt = 0;
    for (size_t k = 0; k < g_cap; k++)
      if (g_conn[k].used && g_conn[k].st == ST_GROUP && !g_conn[k].sub_all &&
          g_conn[k].group_idx == i)
        cnt++;
    char line[96];
    int n = snprintf(line, sizeof(line), "  [%s] %d 个订阅者\n",
                     g_group[i].name, cnt);
    send_to(fd, line, (size_t)n);
  }
  send_str(fd, "输入 SUB <分组> 订阅\n");
}

static void do_stat(int fd) {
  unsigned long long dropped = 0;
  unsigned long long pub = gwup_stats(&dropped);
  char line[256];
  int n = snprintf(line, sizeof(line),
                   "网关统计: 下行帧 %llu / 分发 %llu / 上行发布 %llu / "
                   "上行丢弃 %llu\n",
                   g_frames_in, g_frames_out, pub, dropped);
  send_to(fd, line, (size_t)n);
}

/* 处理一条完整命令/消息（调用方持锁） */
static void handle_line(Conn* c, int fd, char* line) {
  while (*line == ' ' || *line == '\t')
    line++;
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                     line[len - 1] == '\r'))
    line[--len] = '\0';
  if (len == 0)
    return;

  switch (c->st) {
    case ST_REG: {
      if (len >= DEVID_LEN) {
        send_str(fd, "[错误] 设备 ID 太长(≤23字符)，请重新输入:\n");
        return;
      }
      if (strpbrk(line, "\r\n") != NULL) {
        send_str(fd, "[错误] 设备 ID 含非法字符，请重新输入:\n");
        return;
      }
      for (size_t i = 0; i < g_cap; i++) {         /* 设备 ID 查重 */
        if (g_conn[i].used && i != (size_t)fd &&
            strcmp(g_conn[i].devid, line) == 0) {
          send_str(fd, "[错误] 设备 ID 已被占用，换一个:\n");
          return;
        }
      }
      snprintf(c->devid, sizeof(c->devid), "%s", line);
      c->st = ST_MENU;
      char msg[128];
      int n = snprintf(msg, sizeof(msg), "设备 %s 已注册\n", c->devid);
      send_to(fd, msg, (size_t)n);
      print_menu(fd);
      return;
    }
    case ST_MENU:
    case ST_GROUP: {
      if (strlen(line) >= 3 && strncasecmp(line, "SUB", 3) == 0 &&
          (line[3] == '\0' || line[3] == ' ')) {
        do_sub(c, fd, line[3] ? line + 4 : "");
        return;
      }
      if (strcasecmp(line, "UNSUB") == 0) {
        do_unsub(c, fd);
        return;
      }
      if (strncasecmp(line, "DATA ", 5) == 0) {
        /* 设备上报：DATA <key> <value>，来源自动标成 tcp:<devid> */
        char* k = line + 5;
        while (*k == ' ')
          k++;
        char* v = strchr(k, ' ');
        if (v == NULL) {
          send_str(fd, "[错误] 用法: DATA <键> <值>\n");
          return;
        }
        *v++ = '\0';
        while (*v == ' ')
          v++;
        char src[GW_SRC_MAX];
        snprintf(src, sizeof(src), "tcp:%s", c->devid);
        dispatch_locked(src, k, v);       /* 与 CAN/Modbus 同一条出口 */
        pthread_mutex_unlock(&g_mtx);     /* 上行入队放锁外，避免双重加锁 */
        gwup_enqueue(src, k, v);
        pthread_mutex_lock(&g_mtx);
        return;
      }
      if (strcasecmp(line, "LIST") == 0) {
        do_list(fd);
        return;
      }
      if (strcasecmp(line, "STAT") == 0) {
        do_stat(fd);
        return;
      }
      if (strcasecmp(line, "QUIT") == 0 || strcasecmp(line, "/quit") == 0 ||
          strcasecmp(line, "EXIT") == 0 || strcasecmp(line, "/exit") == 0) {
        send_str(fd, "bye\n");
        g_conn[fd].used = -1;             /* 让 gw_on_bytes 返回 1 要求关闭 */
        return;
      }
      if (c->st == ST_GROUP) {
        /* 组内消息：分发给同订阅组的其他连接（面向现场设备的下行命令通道） */
        const char* grp = c->sub_all ? "*" : g_group[c->group_idx].name;
        char msg[LINE_MAX + 96];
        int n = snprintf(msg, sizeof(msg), "[%s] %s: %s\n", grp, c->devid, line);
        if (n <= 0)
          return;
        group_fanout(grp, msg, (size_t)n, fd);
        return;
      }
      send_str(fd, "未知命令。SUB <分组> / DATA <键> <值> / LIST / STAT / QUIT\n");
      return;
    }
  }
}

/* ============================ 对外接口 ============================ */

void gw_init(int cap) {
  g_cap = (size_t)cap;
  for (int i = 0; i < cap; i++) {
    g_conn[i].used = 0;
    g_conn[i].worker = -1;
    g_conn[i].group_idx = -1;
    g_conn[i].sub_all = 0;
  }
  memset(g_group, 0, sizeof(g_group));
  g_group_cnt = 0;
  g_frames_in = g_frames_out = 0;
}

void gw_fd_assign(int fd, int worker) {
  pthread_mutex_lock(&g_mtx);
  if (fd >= 0 && fd < (int)g_cap)
    g_conn[fd].worker = worker;
  pthread_mutex_unlock(&g_mtx);
}

int gw_fd_get(int fd) {
  pthread_mutex_lock(&g_mtx);
  int w = (fd >= 0 && fd < (int)g_cap) ? g_conn[fd].worker : -1;
  pthread_mutex_unlock(&g_mtx);
  return w;
}

void gw_fd_release(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd >= 0 && fd < (int)g_cap) {
    g_conn[fd].worker = -1;
    g_conn[fd].used = 0;
  }
  pthread_mutex_unlock(&g_mtx);
}

void gw_on_accept(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap) {
    pthread_mutex_unlock(&g_mtx);
    return;
  }
  Conn* c = &g_conn[fd];
  c->used = 1;
  c->st = ST_REG;
  c->group_idx = -1;
  c->sub_all = 0;
  c->devid[0] = '\0';
  c->line_len = 0;
  c->overflow = 0;
  pthread_mutex_unlock(&g_mtx);
  send_str(fd, "rk3566 edge gateway ready. 请注册设备 ID:\n");
}

int gw_on_bytes(int fd, const char* buf, size_t len) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap || !g_conn[fd].used) {
    pthread_mutex_unlock(&g_mtx);
    return -1;
  }
  Conn* c = &g_conn[fd];
  int close_req = 0;

  for (size_t i = 0; i < len && !close_req; i++) {
    char ch = buf[i];
    if (ch == '\n') {
      if (c->line_len < LINE_MAX)
        c->line[c->line_len] = '\0';
      if (!c->overflow && c->line_len > 0)
        handle_line(c, fd, c->line);
      if (c->used == -1)
        close_req = 1;
      c->line_len = 0;
      c->overflow = 0;
    } else if (c->line_len < LINE_MAX - 1) {
      c->line[c->line_len++] = ch;
    } else {
      c->overflow = 1; /* 超长行：保留前半截也没意义，标记后整行丢弃 */
    }
  }
  pthread_mutex_unlock(&g_mtx);
  return close_req;
}

void gw_on_close(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap || !g_conn[fd].used) {
    pthread_mutex_unlock(&g_mtx);
    return;
  }
  Conn* c = &g_conn[fd];
  if (c->st == ST_GROUP) {
    const char* grp = c->sub_all ? "*" : g_group[c->group_idx].name;
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "[系统] 设备 %s 已离线\n", c->devid);
    group_fanout(grp, msg, (size_t)n, fd);
  }
  c->used = 0;
  c->worker = -1;
  c->group_idx = -1;
  c->sub_all = 0;
  c->devid[0] = '\0';
  c->line_len = 0;
  c->overflow = 0;
  pthread_mutex_unlock(&g_mtx);
}

void gw_get_stat(unsigned long long* frames_in, unsigned long long* frames_out) {
  pthread_mutex_lock(&g_mtx);
  if (frames_in)
    *frames_in = g_frames_in;
  if (frames_out)
    *frames_out = g_frames_out;
  pthread_mutex_unlock(&g_mtx);
}
