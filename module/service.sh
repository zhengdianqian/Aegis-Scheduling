#!/system/bin/sh
# service.sh — Aegis：仅启动三大功能（eBPF 调度 / 充电阈值 / 温控），不做任何其他系统改动

MODDIR="${0%/*}"
export PATH="$MODDIR/bin/busybox:$PATH"
export MODDIR

chmod 755 "$MODDIR/bin/busybox/busybox" 2>/dev/null
"$MODDIR/bin/busybox/busybox" --install -s "$MODDIR/bin/busybox" 2>/dev/null

# 等开机完成，保证 sysfs/驱动就绪
_wc=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $_wc -lt 60 ]; do
    sleep 3
    _wc=$((_wc + 1))
done
sleep 5

# ── 权限 ──
for _b in thermal_manager update_prop charge_guardd; do
    [ -f "$MODDIR/$_b" ] && chmod 755 "$MODDIR/$_b"
done
chmod 755 "$MODDIR"/charge_guard/*.sh "$MODDIR"/charge_guard/lib/*.sh "$MODDIR"/scripts/*.sh 2>/dev/null
chmod 755 "$MODDIR/core/bin/Aegis" 2>/dev/null

[ -f "$MODDIR/thermal_mode" ] || echo "official" > "$MODDIR/thermal_mode"

# ── 1/3 调度：Aegis eBPF 守护（CLG 负载调频 + FAS 帧调度）──
killall -9 Aegis > /dev/null 2>&1
nohup "$MODDIR/core/bin/Aegis" > /dev/null 2>&1 &

# ── 2/3 温控：按所选档位投放 thermal 配置 ──
(
    "$MODDIR/thermal_manager" "$MODDIR" apply "$(cat "$MODDIR/thermal_mode" 2>/dev/null || echo official)"
) > /dev/null 2>&1 &

# ── 3/3 充电阈值：首次扫描停充节点/机型探测，再拉起守护 ──
(
    CGDIR="$MODDIR/charge_guard"
    mkdir -p "$CGDIR/data"
    [ -f "$CGDIR/data/list_switch" ]    || "$CGDIR/list_switch.sh"   > /dev/null 2>&1
    [ -f "$CGDIR/data/device.profile" ] || "$CGDIR/detect_device.sh" > /dev/null 2>&1
    pkill -f charge_guardd 2>/dev/null
    exec "$MODDIR/charge_guardd" "$MODDIR"
) > /dev/null 2>&1 &

# WebUI 模块描述（当前模式/温控档/充电状态）
"$MODDIR/update_prop" "$MODDIR" 2>/dev/null
exit 0
