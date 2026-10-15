#!/system/bin/sh
# service.sh — Aegis 电源矩阵
#   调度核心：YUMI eBPF 守护（二进制名 Aegis，替换原 YC/uperf）
#   保留组件：充电阈值 charge_guardd / 温控 thermal_manager

MODDIR="${0%/*}"
export PATH="$MODDIR/bin/busybox:$PATH"
export MODDIR

chmod 755 "$MODDIR/bin/busybox/busybox" 2>/dev/null
"$MODDIR/bin/busybox/busybox" --install -s "$MODDIR/bin/busybox" 2>/dev/null

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

# 旧 id 的模块若还在，停用它，避免两套调度/充电守护同时运行
[ -d /data/adb/modules/thread_affinity_opt_8gen2 ] && touch /data/adb/modules/thread_affinity_opt_8gen2/disable
# 若装过独立的 yumi / aegis 调度模块，同样停用，避免双守护抢频率
for _m in yumi aegis; do
    [ -d "/data/adb/modules/$_m" ] && touch "/data/adb/modules/$_m/disable"
done

# ── 默认标志文件 ──
[ -f "$MODDIR/thermal_mode" ] || echo "official" > "$MODDIR/thermal_mode"

# ── 切换调度核心：彻底关停旧 YC/uperf（Aegis 接管全部 cpufreq 调频）──
pkill -9 -f uperf 2>/dev/null
pkill -9 -f "kern_opt daemon" 2>/dev/null
[ -x "$MODDIR/kern_opt" ] && "$MODDIR/kern_opt" stop >/dev/null 2>&1

# ── 一次性：清理旧版本遗留的、已无人管理的系统状态 ──
if [ ! -f "$MODDIR/.legacy_cleaned_v3" ]; then
    pkill -f charge_monitor 2>/dev/null
    for _f in fast_charge temp_spoof_enabled cycle_spoof_enabled soh_spoof_enabled \
              freq_limit_enabled freq_limit_mode freq_limit_custom freq_limit_profiles \
              freq_limit_profile_1 freq_limit_profile_2 freq_limit_profile_3 freq_mode \
              cloud_ctrl_enabled sys_opt kern_opt_old charge_monitor charge_once \
              yc_enabled yc_mode; do
        rm -rf "$MODDIR/$_f" 2>/dev/null
    done
    rm -rf "$MODDIR/sys_opt_snapshot" "$MODDIR/gpu_gov_baseline" 2>/dev/null
    [ -f /sys/class/qcom-battery/fake_temp ] && echo 0 > /sys/class/qcom-battery/fake_temp 2>/dev/null

    _rp=""
    for _c in resetprop /data/adb/magisk/resetprop /data/adb/ksu/bin/resetprop; do
        if command -v "$_c" > /dev/null 2>&1 || [ -x "$_c" ]; then _rp="$_c"; break; fi
    done
    if [ -n "$_rp" ]; then
        for _p in persist.traced.enable persist.heapprofd.enable persist.traced_perf.enable \
                  persist.mm.enable.prefetch persist.sys.preload.enable persist.sys.prestart.proc \
                  persist.sys.prestart.feedback.enable persist.sys.launch_response_optimization.enable \
                  persist.sys.stability.MultiCacheEnable persist.sys.stability.SmartCacheEnable \
                  persist.vendor.qti.binder_opt.enable persist.radio.disable_power_collapse \
                  persist.radio.disable_flexmapping wifi.supplicant_scan_interval \
                  persist.sys.wifi.scan_always_enabled persist.sys.scheduler.uclamp \
                  persist.sys.scheduler.rtmode job.max_run_active_jobs_count \
                  persist.sys.anr.sfwatchdog.is_fatal persist.sys.analytics.disabled \
                  persist.sys.ad.enabled persist.xiaomi.experience_plan persist.miui.cloud.control \
                  persist.sys.anim.fullscreen_resolve persist.bluetooth.disable_abs_vol; do
            "$_rp" -p -d "$_p" > /dev/null 2>&1
        done
    fi

    # 旧 miui_migt.sh 写进 settings 库的值：重启/卸载都不会回滚
    if [ "$(settings get secure speed_mode_enable 2>/dev/null)" = "1" ]; then
        settings delete secure speed_mode_enable > /dev/null 2>&1
    fi
    # YC 时代的 migt/glk 遮罩缓存
    rm -rf /cache/sys/module/migt /cache/proc/sys/glk /cache/proc/sys/migt 2>/dev/null
    touch "$MODDIR/.legacy_cleaned_v3"
fi

# ── logd 静默 ──
# 本机日志流量异常（~2 万条/秒厂商垃圾日志），logd 常驻空耗 CPU，默认停掉。
# 需要日志时：setprop persist.sys.aegis.keep_logd 1 后重启。
if [ "$(getprop persist.sys.aegis.keep_logd)" != "1" ]; then
    stop logd
fi

# ── Aegis 调度守护（yumi eBPF 核心：CLG 负载调速 + FAS 按需帧调度）──
killall -9 Aegis > /dev/null 2>&1
killall -9 yumi  > /dev/null 2>&1
nohup "$MODDIR/core/bin/Aegis" > /dev/null 2>&1 &

# ── 温控 ──
(
    "$MODDIR/thermal_manager" "$MODDIR" apply "$(cat "$MODDIR/thermal_mode" 2>/dev/null || echo official)"
) > /dev/null 2>&1 &

# ── 充电阈值：首次扫描停充节点 / 机型探测，再拉起守护进程 ──
(
    CGDIR="$MODDIR/charge_guard"
    mkdir -p "$CGDIR/data"
    [ -f "$CGDIR/data/list_switch" ]    || "$CGDIR/list_switch.sh"   > /dev/null 2>&1
    [ -f "$CGDIR/data/device.profile" ] || "$CGDIR/detect_device.sh" > /dev/null 2>&1
    pkill -f charge_guardd 2>/dev/null
    exec "$MODDIR/charge_guardd" "$MODDIR"
) > /dev/null 2>&1 &

"$MODDIR/update_prop" "$MODDIR" 2>/dev/null
exit 0
