#!/bin/bash
# 端到端冒烟：起网关 -> 3 个模拟设备上报 -> 订阅端应原样收齐
#   用法（仓库根目录）：bash scripts/smoke_test.sh
# 不依赖 CAN / 串口 / MQTT broker / 内核模块 —— 缺了它们网关会自动降级，
# 本脚本顺便验证这四条降级路径不会拖垮服务。
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
[ -x build/gwgateway ] || { echo "[FAIL] 先跑 cmake --build build"; exit 1; }

GWLOG=$(mktemp); WLOG=$(mktemp); SIMLOG=$(mktemp)
stdbuf -o0 -e0 ./build/gwgateway > "$GWLOG" 2>&1 & GW=$!
sleep 1
timeout 20 ./build/gwclient watch tcp > "$WLOG" 2>&1 & W=$!
sleep 1
./build/gwclient sim 3 > "$SIMLOG" 2>&1      # 3 设备 x 10 轮 = 30 帧
sleep 1
kill $W 2>/dev/null; kill $GW 2>/dev/null; sleep 0.3

GOT=$(grep -c '^DATA tcp:' "$WLOG")
echo "--- 网关通道状态 ---"; grep -E '\[WARN\]|\[INFO\] (内核|未配置|CAN|Modbus|edge)' "$GWLOG"
echo "--- 订阅端收到 DATA 帧: $GOT（期望 30）---"
grep '^DATA' "$WLOG" | head -3
rm -f "$GWLOG" "$WLOG" "$SIMLOG"
[ "$GOT" -eq 30 ] && { echo "[PASS] 端到端 30/30"; exit 0; } || { echo "[FAIL] 应为 30，实收 $GOT"; exit 1; }
