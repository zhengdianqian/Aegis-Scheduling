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

# 仅处理本模块自身的升级（保留用户设置），不触碰任何其他模块
OLD="/data/adb/modules/aegis_power_matrix"

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

# ── 刷入即适配：探测本机 CPU 分簇/capacity，生成簇权重与 perf 范围。
#    任何失败都回退包内定值（8Gen2 v3 参数），绝不阻断刷入。──
ADAPT_WORK="${TMPDIR:-/data/local/tmp}"
AR="$ADAPT_WORK/aegis_rules_new.yaml"
AC="$ADAPT_WORK/aegis_config_new.yaml"
ALOG="$ADAPT_WORK/aegis_adapt.log"
rm -f "$AR" "$AC" "$ALOG" "$ADAPT_WORK/aegis_probe_data.txt"*
ADAPT_OK=0
if AEGIS_WORKDIR="$ADAPT_WORK" sh "$MODPATH/tools/aegis_adapt.sh" "$MODPATH" install >"$ALOG" 2>&1; then
    if [ -s "$AR" ] && [ -s "$AC" ]; then ADAPT_OK=1; fi
fi
if [ "$ADAPT_OK" = 1 ]; then
    NCL=$(grep '^AEGIS_ADAPT_NCLUSTER=' "$ALOG" | head -1 | cut -d= -f2)
    ui_print "- 硬件适配成功：探测到 ${NCL:-?} 个 CPU 簇"
    grep '^AEGIS_ADAPT_POLICY=' "$ALOG" | while IFS='=' read -r _line; do
        _p="${_line#AEGIS_ADAPT_POLICY=}"
        ui_print "    policy${_p%%|*} 权重 $(echo "$_p" | awk -F'|' '{print $NF}')"
    done
    # 调度规则：保留用户的模式/FAS开关/每应用规则，fas_rules 换成本机适配段
    if [ -f "$OLD/rules.yaml" ]; then
        USER_HEAD="$ADAPT_WORK/rules_user_head.tmp"
        NEW_FAS="$ADAPT_WORK/rules_new_fas.tmp"
        # serde 写回时 fas_rules 恒为末段
        awk 'BEGIN{p=1} /^fas_rules:/{p=0} p{print}' "$OLD/rules.yaml" > "$USER_HEAD"
        awk 'BEGIN{p=0} /^fas_rules:/{p=1} p{print}' "$AR" > "$NEW_FAS"
        cat "$USER_HEAD" "$NEW_FAS" > "$MODPATH/rules.yaml"
        rm -f "$USER_HEAD" "$NEW_FAS"
        ui_print "- 已保留模式/FAS开关/应用规则"
    else
        cp -f "$AR" "$MODPATH/rules.yaml"
    fi
    cp -f "$AC" "$MODPATH/config/config.yaml"
else
    ui_print "- ! 自动适配未成功，沿用包内 8Gen2 定值（可用 tools/Aegis_一键适配.sh 手动适配）"
    # 回退：包内 rules.yaml 的 fas_rules 与旧版用户段合并
    if [ -f "$OLD/rules.yaml" ]; then
        USER_HEAD="$ADAPT_WORK/rules_user_head.tmp"
        NEW_FAS="$ADAPT_WORK/rules_new_fas.tmp"
        awk 'BEGIN{p=1} /^fas_rules:/{p=0} p{print}' "$OLD/rules.yaml" > "$USER_HEAD"
        awk 'BEGIN{p=0} /^fas_rules:/{p=1} p{print}' "$MODPATH/rules.yaml" > "$NEW_FAS"
        cat "$USER_HEAD" "$NEW_FAS" > "$MODPATH/rules.yaml"
        rm -f "$USER_HEAD" "$NEW_FAS"
        ui_print "- 已保留模式/FAS开关/应用规则，FAS 内核参数已更新"
    fi
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
set_perm "$MODPATH/tools/aegis_adapt.sh" 0 0 0755

ui_print ""
ui_print "  🛡️ Aegis"
ui_print "  · eBPF 调度（FAS 零空转）· 充电阈值 · 温控"
ui_print "  重启后生效"
