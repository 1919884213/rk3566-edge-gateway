#!/bin/bash
# gwcache x 网关：端到端集成验收。用法（在仓库根目录）：
#   sudo MOD=~/gwcache-build/gwcache/gwcache.ko bash scripts/gwcache_integ.sh
# 依赖：先跑过 cmake --build build（需要 build/gwgateway 与 build/gwcachemon）
U="${SUDO_USER:-$(id -un)}"; H="$(getent passwd "$U" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MOD="${MOD:-$H/gwcache-build/gwcache/gwcache.ko}"
pass=0; fail=0
ok(){ echo "  [PASS] $1"; pass=$((pass+1)); }
no(){ echo "  [FAIL] $1"; fail=$((fail+1)); }

[ -f "$MOD" ] || { echo "[FAIL] 没有模块文件 $MOD"; exit 1; }
insmod "$MOD" buf_kb=64 && ok "驱动加载" || { echo "insmod 失败"; exit 1; }
sleep 0.3

"$HERE/build/gwcachemon" > /tmp/integ_snap.log 2>&1 &
MONPID=$!
sleep 0.3

stdbuf -o0 -e0 "$HERE/build/gwgateway" > /tmp/integ_gw.log 2>&1 &
GWPID=$!
sleep 1
grep -q "内核缓存通道 -> /dev/gwcache" /tmp/integ_gw.log && ok "网关已接上内核缓存通道" || no "网关没接上通道"
sed 's/^/    /' /tmp/integ_gw.log

echo "===== 模拟现场设备：注册 + 订阅 + 上报（唯一会产生分发帧的路径）====="
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 8080), timeout=3)
s.settimeout(3)
time.sleep(0.4); s.sendall(b"itest_plc\n")      # 一行 = 注册设备 ID
time.sleep(0.4); s.sendall(b"SUB tcp\n")        # 订阅 tcp 分组
time.sleep(0.4); s.sendall(b"DATA temp 26\n")   # 上报一条遥测
time.sleep(0.6); s.close()
print("  设备已发送：注册 -> SUB tcp -> DATA temp 26")
PY
sleep 0.5

ST=$("$HERE/build/gwcachemon" -s); echo "    驱动统计: $ST"
echo "$ST" | grep -qE "in=[1-9]" && ok "驱动确实收到写入（in>0）" || no "驱动 in=0，没有数据进来"
grep -q "DATA tcp:itest_plc temp 26" /tmp/integ_snap.log && ok "监控端从内核读回网关数据帧（端到端成立）" || no "监控端没读到内容"
echo "  --- 监控端抓到的内容 ---"; sed 's/^/    /' /tmp/integ_snap.log | head -10

echo "===== 收尾 ====="
kill $GWPID $MONPID 2>/dev/null; wait 2>/dev/null
rmmod gwcache && ok "卸载干净" || no "卸载失败"

echo; echo "===== 汇总：PASS $pass / FAIL $fail ====="
