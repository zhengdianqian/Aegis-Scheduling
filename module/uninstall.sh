#!/system/bin/sh
# uninstall.sh — Aegis 卸载回滚：只回滚本模块三大功能（调度/充电阈值/温控）产生的改动
MODDIR="${0%/*}"

# 1) 调度守护停止并恢复 CPU  governor 为系统默认（walt/schedutil）
killall -9 Aegis 2>/dev/null
# 等待守护完全退出
sleep 1
# 恢复所有 policy 的 governor 为系统默认（优先 walt，不存在则用 schedutil）
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -d "$policy" ]; then
        if [ -f "$policy/scaling_available_governors" ]; then
            if grep -q "walt" "$policy/scaling_available_governors" 2>/dev/null; then
                echo "walt" > "$policy/scaling_governor" 2>/dev/null
            elif grep -q "schedutil" "$policy/scaling_available_governors" 2>/dev/null; then
                echo "schedutil" > "$policy/scaling_governor" 2>/dev/null
            fi
        fi
        # 恢复频率范围为硬件默认（min=最小，max=最大）
        if [ -f "$policy/cpuinfo_min_freq" ] && [ -f "$policy/scaling_min_freq" ]; then
            cat "$policy/cpuinfo_min_freq" > "$policy/scaling_min_freq" 2>/dev/null
        fi
        if [ -f "$policy/cpuinfo_max_freq" ] && [ -f "$policy/scaling_max_freq" ]; then
            cat "$policy/cpuinfo_max_freq" > "$policy/scaling_max_freq" 2>/dev/null
        fi
    fi
done

# 2) 充电阈值：先终止守护进程，再还原节点（避免守护在 restore 后再次停充）
pkill -f charge_guardd 2>/dev/null
sleep 0.5
# 守护进程自还原，失败再走脚本兜底
if [ -x "$MODDIR/charge_guardd" ]; then
    "$MODDIR/charge_guardd" "$MODDIR" restore 2>/dev/null
fi
# 强制恢复充电控制节点（防止残留）
for node in /sys/class/power_supply/battery/charge_control_limit /sys/class/power_supply/battery/input_suspend; do
    if [ -w "$node" ]; then
        case "$node" in
            *charge_control_limit)
                # 恢复为最大值（不限制充电）
                cat /sys/class/power_supply/battery/charge_control_limit_max > "$node" 2>/dev/null || echo 16 > "$node" 2>/dev/null
                ;;
            *input_suspend)
                echo 0 > "$node" 2>/dev/null
                ;;
        esac
    fi
done
if [ -f "$MODDIR/charge_guard/common.sh" ]; then
    . "$MODDIR/charge_guard/common.sh" 2>/dev/null
    zg_restore_switches_from_list 2>/dev/null || true
    zg_restore_mca_charge 2>/dev/null || true
fi

# 4) 屏幕超时恢复（息屏时守护会改成 10 秒，卸载时还原为 30 秒默认）
settings put system screen_off_timeout 30000 2>/dev/null

# 5) 温控：删除模块投放的 vendor thermal 配置并恢复目录属性
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
