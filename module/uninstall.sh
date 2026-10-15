#!/system/bin/sh
# uninstall.sh — Aegis 电源模块卸载回滚
MODDIR="${0%/*}"

# 0) 停掉调度守护
killall -9 Aegis 2>/dev/null
killall -9 yumi 2>/dev/null
pkill -9 -f uperf 2>/dev/null
pkill -f "kern_opt daemon" 2>/dev/null
[ -x "$MODDIR/kern_opt" ] && "$MODDIR/kern_opt" stop 2>/dev/null

# 1) 停充节点还原为「充电」（先让守护进程自己还原，失败再走脚本）
if [ -x "$MODDIR/charge_guardd" ]; then
    "$MODDIR/charge_guardd" "$MODDIR" restore 2>/dev/null
fi
pkill -f charge_guardd 2>/dev/null
if [ -f "$MODDIR/charge_guard/common.sh" ]; then
    . "$MODDIR/charge_guard/common.sh" 2>/dev/null
    zg_restore_switches_from_list 2>/dev/null || true
    zg_restore_mca_charge 2>/dev/null || true
fi

# 2) 温控配置回滚（删除模块投放的 vendor thermal conf）
THERMAL_DATA="/data/vendor/thermal/config"
if [ -d "$THERMAL_DATA" ]; then
    chattr -i "$THERMAL_DATA" 2>/dev/null
    find "$THERMAL_DATA" -name "*.conf" -exec chattr -i {} \; 2>/dev/null
    find "$THERMAL_DATA" -name "*.conf" -delete 2>/dev/null
    chmod -R 0771 /data/vendor/thermal 2>/dev/null
    chown -R root:system "$THERMAL_DATA" 2>/dev/null
    restorecon -DFR /data/vendor/thermal 2>/dev/null
fi

# 3) 旧版本遗留：fake_temp / 被冻结的包 / settings 值
[ -f /sys/class/qcom-battery/fake_temp ] && echo 0 > /sys/class/qcom-battery/fake_temp 2>/dev/null
for _pkg in com.miui.hybrid com.miui.bugreport com.android.providers.partnerbookmarks \
            com.android.bips com.android.printspooler com.google.android.printservice.recommendation \
            com.android.cellbroadcastreceiver com.android.cellbroadcastservice com.android.egg \
            com.android.traceur com.android.ondevicepersonalization.services \
            com.android.federatedcompute.services com.miui.analytics com.android.adservices.api; do
    pm enable --user 0 "$_pkg" > /dev/null 2>&1
done
if [ "$(settings get secure speed_mode_enable 2>/dev/null)" = "1" ]; then
    settings delete secure speed_mode_enable > /dev/null 2>&1
fi

rm -rf /storage/emulated/0/Android/yc 2>/dev/null
rm -rf /cache/sys/module/migt /cache/proc/sys/glk /cache/proc/sys/migt 2>/dev/null
exit 0
