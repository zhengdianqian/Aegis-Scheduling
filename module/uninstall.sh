#!/system/bin/sh
# uninstall.sh — Aegis 卸载回滚：只回滚本模块三大功能（调度/充电阈值/温控）产生的改动
MODDIR="${0%/*}"

# 1) 调度守护停止（cpufreq 调频为内核运行时状态，重启后自动恢复系统默认）
killall -9 Aegis 2>/dev/null

# 2) 充电阈值：把停充节点还原为「允许充电」（先守护进程自还原，失败再走脚本表）
if [ -x "$MODDIR/charge_guardd" ]; then
    "$MODDIR/charge_guardd" "$MODDIR" restore 2>/dev/null
fi
pkill -f charge_guardd 2>/dev/null
if [ -f "$MODDIR/charge_guard/common.sh" ]; then
    . "$MODDIR/charge_guard/common.sh" 2>/dev/null
    zg_restore_switches_from_list 2>/dev/null || true
    zg_restore_mca_charge 2>/dev/null || true
fi

# 3) 温控：删除模块投放的 vendor thermal 配置并恢复目录属性
THERMAL_DATA="/data/vendor/thermal/config"
if [ -d "$THERMAL_DATA" ]; then
    chattr -i "$THERMAL_DATA" 2>/dev/null
    find "$THERMAL_DATA" -name "*.conf" -exec chattr -i {} \; 2>/dev/null
    find "$THERMAL_DATA" -name "*.conf" -delete 2>/dev/null
    chmod -R 0771 /data/vendor/thermal 2>/dev/null
    chown -R root:system "$THERMAL_DATA" 2>/dev/null
    restorecon -DFR /data/vendor/thermal 2>/dev/null
fi

exit 0
