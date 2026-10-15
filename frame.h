#ifndef GW_FRAME_H
#define GW_FRAME_H

/*
 * 网关统一数据帧（内部表示）。
 *
 * 线上/内部都用一行文本，三种下行源收敛成同一格式，后面才能走同一条
 * 「缓存 + 上报 + 分组分发」路径：
 *
 *     DATA <source> <key> <value>\n
 *
 *   source  can:0x100         CAN 帧，来源是 CAN ID
 *           modbus:1           Modbus 从站地址
 *           tcp:<devid>        TCP 设备（设备自己注册的 ID）
 *   key     信号名 / 点位名（CAN 侧查 ID->信号名映射表，未命中为 raw）
 *   value   字符串化的值（CAN 为负载 hex，Modbus 为寄存器十进制）
 *
 * 分组（group）取 source 的前缀：can / modbus / tcp。
 * TCP 客户端用 SUB <group> 订阅某个分组，用 '*' 订阅全部。
 */

#define GW_FRAME_PREFIX "DATA "
#define GW_SRC_MAX     32
#define GW_KEY_MAX     32
#define GW_VAL_MAX     64
#define GW_LINE_MAX    160

typedef enum { GW_GROUP_CAN = 0, GW_GROUP_MODBUS, GW_GROUP_TCP, GW_GROUP_ANY } GwGroup;

/* 下行源把一帧交给网关核心的统一入口（由 main 装配注入） */
typedef void (*gw_frame_sink)(const char* source, const char* key,
                              const char* value);

#endif /* GW_FRAME_H */
