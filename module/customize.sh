#!/system/bin/sh
# customize.sh — Aegis（eBPF 调度 / 充电阈值 / 温控）

SKIPUNZIP=0
ASH_STANDALONE=0

ui_print "- ************************"
ui_print "-           Aegis"
ui_print "-   eBPF 调度 · FAS 零空转"
ui_print "-   充电阈值 · 三档温控"
ui_print "- ************************"

case "$ARCH" in
    arm64) BIN_DIR="$MODPATH/bin/arm64-v8a" ;;
    arm)   abort "! Aegis 核心仅支持 arm64，32 位设备无法安装" ;;
    *)     abort "! 不支持的架构: $ARCH" ;;
esac
ui_print "- 设备架构: $ARCH"

NEWDIR="/data/adb/modules/aegis_power_matrix"
LEGACY="/data/adb/modules/thread_affinity_opt_8gen2"
OLD="$NEWDIR"
if [ ! -d "$OLD" ] && [ -d "$LEGACY" ]; then
    OLD="$LEGACY"
    ui_print "- 检测到旧版模块，从它继承设置"
fi
# 旧版不能和新版同时运行
[ -d "$LEGACY" ] && touch "$LEGACY/disable"
# 若装过独立的 yumi / aegis 调度模块，停用以避免双守护抢频率
for _m in yumi aegis; do
    [ -d "/data/adb/modules/$_m" ] && touch "/data/adb/modules/$_m/disable"
done

# ── 复制核心二进制（保留 bin/busybox；arm64-v8a 副本复制后清理）──
for _bin in update_prop thermal_manager charge_guardd; do
    [ -f "$BIN_DIR/$_bin" ] || abort "! 缺少核心程序: $_bin ($ARCH)"
    cp "$BIN_DIR/$_bin" "$MODPATH/$_bin"
    chmod a+x "$MODPATH/$_bin"
done
"$MODPATH/charge_guardd" -v > /dev/null 2>&1 || abort "! charge_guardd 验证失败"
rm -rf "$MODPATH/bin/arm64-v8a"

# Aegis 调度守护（yumi eBPF 核心）
[ -f "$MODPATH/core/bin/Aegis" ] || abort "! 缺少 Aegis 调度核心"
chmod 755 "$MODPATH/bin/busybox/busybox"
"$MODPATH/bin/busybox/busybox" --install -s "$MODPATH/bin/busybox" 2>/dev/null

# ── 温控档位继承 ──
[ -f "$OLD/thermal_mode" ] && cp "$OLD/thermal_mode" "$MODPATH/thermal_mode"
[ -f "$MODPATH/thermal_mode" ] || echo "official" > "$MODPATH/thermal_mode"

# ── 调度规则继承：保留用户的模式选择/FAS 开关/每应用规则，
#    但 fas_rules 内核段替换为包内的 SoC 定制值（如三簇算力权重）──
if [ -f "$OLD/rules.yaml" ]; then
    USER_HEAD="$MODPATH/.rules_user_head.tmp"
    NEW_FAS="$MODPATH/.rules_new_fas.tmp"
    # 旧文件 fas_rules: 之前的个人配置段（serde 写回时 fas_rules 恒为末段）
    awk 'BEGIN{p=1} /^fas_rules:/{p=0} p{print}' "$OLD/rules.yaml" > "$USER_HEAD"
    # 新包文件的 fas_rules 整段
    awk 'BEGIN{p=0} /^fas_rules:/{p=1} p{print}' "$MODPATH/rules.yaml" > "$NEW_FAS"
    cat "$USER_HEAD" "$NEW_FAS" > "$MODPATH/rules.yaml"
    rm -f "$USER_HEAD" "$NEW_FAS"
    ui_print "- 已保留模式/FAS开关/应用规则，FAS 内核参数已更新"
fi

# ── 充电阈值：保留配置与开关状态，全新安装默认关闭（去 WebUI 手动开启）──
mkdir -p "$MODPATH/charge_guard/data"
if [ -f "$OLD/charge_guard.conf" ]; then
    cp -f "$OLD/charge_guard.conf" "$MODPATH/charge_guard.conf"
    ui_print "- 已保留充电阈值配置"
fi
if [ -f "$OLD/charge_guard/data/off" ] || [ ! -f "$OLD/charge_guard.conf" ]; then
    touch "$MODPATH/charge_guard/data/off"
fi
for _d in list_switch device.profile; do
    [ -f "$OLD/charge_guard/data/$_d" ] && cp -f "$OLD/charge_guard/data/$_d" "$MODPATH/charge_guard/data/$_d"
done

# ── 权限 ──
set_perm_recursive "$MODPATH" 0 0 0755 0644
for _x in update_prop thermal_manager charge_guardd service.sh uninstall.sh customize.sh; do
    set_perm "$MODPATH/$_x" 0 0 0755
done
set_perm "$MODPATH/bin/busybox/busybox" 0 0 0755
set_perm_recursive "$MODPATH/charge_guard" 0 0 0755 0755
set_perm "$MODPATH/charge_guard.conf" 0 0 0644
set_perm "$MODPATH/core/bin/Aegis" 0 0 0755
set_perm "$MODPATH/scripts/disable_boost.sh" 0 0 0755

ui_print ""
ui_print "  🛡️ Aegis"
ui_print "  · eBPF 调度（FAS 零空转）· 充电阈值 · 温控"
ui_print "  重启后生效"
