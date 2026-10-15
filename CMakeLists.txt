cmake_minimum_required (VERSION 3.10)

project(rk3566_edge_gateway C)   # 纯 C 项目：只启用 C，免掉交叉编译时对 g++ 的依赖

# 网关主程序：Reactor + 网关核心 + 两条下行 + 一条上行 + 内核缓存旁路
add_executable(gwgateway
    main.c
    Server/Server.c
    Server/gwcache_hook.c
    Gateway/Gateway.c
    Downlink/can.c
    Downlink/modbus.c
    Uplink/mqtt.c
    pool/pool.c
)

# 设备模拟客户端（模拟现场 TCP 设备，也用来压连通性）
add_executable(gwclient
    Client/Client.c
)

target_include_directories(gwgateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/Downlink
    ${CMAKE_CURRENT_SOURCE_DIR}/Uplink
    ${CMAKE_CURRENT_SOURCE_DIR}/pool
)

target_include_directories(gwclient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Client
)

# 内核缓存监控端：验证 /dev/gwcache 的 poll/read/ioctl
add_executable(gwcachemon
    tools/gwcache_mon.c
)
target_include_directories(gwcachemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/gwcache
)

# 网关核心自检：把临时文件当 socket，驱动 gw_on_bytes，覆盖
# 注册 -> 订阅 -> 上报 -> 分组分发 -> 退订 全链路，并校验 CAN/Modbus 两个下行转换
add_executable(gwtest
    build_test/test_gateway.c
    Gateway/Gateway.c
    Server/gwcache_hook.c
    Uplink/mqtt.c
)
target_include_directories(gwtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/Downlink
    ${CMAKE_CURRENT_SOURCE_DIR}/Uplink
)

# 纯逻辑自检：Modbus CRC16 + MQTT 报文编码（不依赖 socket，可在任意平台跑）
add_executable(gwlogic_test
    build_test/test_logic.c
)
target_include_directories(gwlogic_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Downlink
    ${CMAKE_CURRENT_SOURCE_DIR}/Uplink
)

find_package(Threads REQUIRED)
target_link_libraries(gwgateway PRIVATE Threads::Threads)
target_link_libraries(gwclient  PRIVATE Threads::Threads)
target_link_libraries(gwtest    PRIVATE Threads::Threads)
