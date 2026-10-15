#!/system/bin/sh
# 充电节点写入：用户 power_switch + 通用 fallback + MCA / preferred 优先

# 各机型常见节点兜底（与 list_switch 扫描互补）
# 各机型常见停充节点；运行时仍以文件是否存在为准。
# 值里空格用 _ 表示（写入前还原）。
ZG_FALLBACK_SWITCHES="\
/sys/class/power_supply/battery/charge_charger_state,start=1,stop=0 \
/sys/class/power_supply/battery/connect_disable,start=0,stop=1 \
/sys/class/power_supply/battery/batt_slate_mode,start=0,stop=1 \
/sys/class/power_supply/battery/store_mode,start=0,stop=1 \
/sys/class/power_supply/battery/input_suspend,start=0,stop=1 \
/sys/class/power_supply/battery/battery_input_suspend,start=0,stop=1 \
/sys/class/power_supply/battery/night_charging,start=0,stop=1 \
/sys/class/power_supply/battery/charging_enabled,start=1,stop=0 \
/sys/class/power_supply/battery/battery_charging_enabled,start=1,stop=0 \
/sys/class/power_supply/battery/batt_charging_enabled,start=1,stop=0 \
/sys/class/power_supply/battery/charge_disable,start=0,stop=1 \
/sys/class/power_supply/battery/disable_charging,start=0,stop=1 \
/sys/class/power_supply/battery/stop_charging,start=0,stop=1 \
/sys/class/power_supply/battery/charge_enabled,start=1,stop=0 \
/sys/class/power_supply/battery/op_disable_charge,start=0,stop=1 \
/sys/class/power_supply/battery/device/Charging_Enable,start=1,stop=0 \
/sys/class/power_supply/battery/ChargerEnable,start=1,stop=0 \
/sys/class/power_supply/battery/force_disable_charging,start=0,stop=1 \
/sys/class/power_supply/battery/charge_control_enabled,start=1,stop=0 \
/sys/class/power_supply/battery/mi_charge_enable,start=1,stop=0 \
/sys/class/power_supply/battery_ext/smart_charging_interruption,start=0,stop=1 \
/sys/class/power_supply/main/adapter_cc_mode,start=0,stop=1 \
/sys/class/power_supply/main/cool_mode,start=0,stop=1 \
/sys/class/power_supply/charger/charge_disable,start=0,stop=1 \
/sys/class/power_supply/bms/charge_disable,start=0,stop=1 \
/sys/class/power_supply/bms/charging_enabled,start=1,stop=0 \
/sys/class/power_supply/bms/charge_enabled,start=1,stop=0 \
/sys/class/power_supply/mi_chg/charge_disable,start=0,stop=1 \
/sys/class/power_supply/mi_chg/charging_enabled,start=1,stop=0 \
/sys/class/qcom-battery/charging_enabled,start=1,stop=0 \
/sys/class/qcom-battery/charge_disable,start=0,stop=1 \
/sys/class/qcom-battery/input_suspend,start=0,stop=1 \
/sys/class/qcom-battery/battery_charging_enabled,start=1,stop=0 \
/sys/class/qcom-battery/charging_suspend_battery,start=0,stop=1 \
/sys/class/qcom-battery/night_charging,start=0,stop=1 \
/sys/class/qcom-battery/cool_mode,start=0,stop=1 \
/sys/class/qcom-battery/batt_protect_en,start=0,stop=1 \
/sys/class/asuslib/charger_limit_en,start=0,stop=1 \
/sys/class/asuslib/charging_suspend_en,start=0,stop=1 \
/sys/class/hw_power/charger/charge_data/enable_charger,start=1,stop=0 \
/sys/class/power_supply/idt/pin_enabled,start=1,stop=0 \
/sys/devices/platform/huawei_charger/enable_charger,start=1,stop=0 \
/sys/devices/platform/lge-unified-nodes/charging_enable,start=1,stop=0 \
/sys/devices/platform/lge-unified-nodes/charging_completed,start=0,stop=1 \
/sys/devices/platform/mt-battery/disable_charger,start=0,stop=1 \
/sys/devices/platform/battery/ChargerEnable,start=1,stop=0 \
/sys/devices/platform/soc/soc:google,charger/charge_disable,start=0,stop=1 \
/sys/devices/platform/charger/bypass_charger,start=0,stop=1 \
/sys/devices/platform/soc/soc:qcom,pmic_glink/soc:qcom,pmic_glink:qcom,battery_charger/force_charger_suspend,start=0,stop=1 \
/sys/devices/virtual/oplus_chg/battery/mmi_charging_enable,start=1,stop=0 \
/sys/kernel/debug/google_charger/chg_suspend,start=0,stop=1 \
/sys/kernel/debug/google_charger/input_suspend,start=0,stop=1 \
/sys/kernel/debug/google_charger/chg_mode,start=1,stop=0 \
/sys/kernel/nubia_charge/charger_bypass,start=off,stop=on \
/sys/module/qpnp_adaptive_charge/parameters/blocking,start=0,stop=1 \
/proc/driver/charger_limit_enable,start=0,stop=1 \
/proc/driver/charger_limit,start=100,stop=1 \
/proc/mtk_battery_cmd/current_cmd,start=0_0,stop=0_1 \
/proc/mtk_battery_cmd/en_power_path,start=1,stop=0 \
/sys/class/power_supply/battery/constant_charge_current_max,start=3000000,stop=0 \
/sys/class/power_supply/battery/current_max,start=3000000,stop=0 \
/sys/class/power_supply/battery/input_current_max,start=3000000,stop=0 \
/sys/class/power_supply/battery/charge_current,start=3000000,stop=0 \
/sys/class/power_supply/battery/fast_charge_current_max,start=3000000,stop=0 \
/sys/class/power_supply/usb/input_suspend,start=0,stop=1 \
/sys/class/power_supply/qc_usb/input_suspend,start=0,stop=1 \
/sys/class/power_supply/dc/input_suspend,start=0,stop=1 \
/sys/class/power_supply/ac/device/power_supply/usb/power_switch,start=1,stop=0 \
/sys/class/power_supply/ac/device/power_supply/usb/input_suspend,start=0,stop=1 \
/sys/class/power_supply/battery/charge_control_end_threshold,start=100,stop=0 \
/sys/class/power_supply/battery/charge_type,start=Fast,stop=None \
/sys/class/power_supply/pc_port/input_suspend,start=0,stop=1 \
/sys/class/power_supply/wireless/input_suspend,start=0,stop=1"

ZG_USER_SWITCHES=""

zg_write_node() {
	local node="$1"
	local val="$2"
	local key got
	chmod 0644 "$node" 2>/dev/null
	if ! echo "$val" > "$node" 2>/dev/null; then
		key="$(echo "$node" | tr / _)"
		zg_log_once "wn_$key" warn "写入节点失败 $node ← $val"
		return 1
	fi
	# ★ 写后回读校验：部分节点（实测如 kalama 的 charging_suspend_battery）
	#   write() 返回成功但内核根本不生效（读回仍是旧值），不校验的话
	#   WebUI/守护会误以为停充成功。内核可能回显规范化分隔符（tab/逗号），
	#   比较前做空白归一；读不回内容的节点跳过校验（由 C 守护的
	#   write_node 回读逻辑兜底）。
	got="$(cat "$node" 2>/dev/null)"
	if [ -n "$got" ] && [ "$(echo $got)" != "$(echo $val)" ]; then
		key="$(echo "$node" | tr / _)"
		zg_log_once "wnv_$key" warn "节点写入未生效（回读不一致）$node want=$val got=$got"
		return 1
	fi
}

# 将一行配置规范为 path,start=X,stop=Y（兼容 [] 与 :: 空格写法）
zg_normalize_power_switch_entry() {
	local raw="$1"
	local body path start stop
	raw="$(echo "$raw" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
	[ -n "$raw" ] || return 1
	case "$raw" in
		\#*) return 1 ;;
	esac
	# power_switch=[...] 或 power_switch=...
	case "$raw" in
		power_switch=\[*\])
			body="$(echo "$raw" | sed 's/^power_switch=\[//;s/\]$//')"
			;;
		power_switch=*)
			body="$(echo "$raw" | sed 's/^power_switch=//')"
			;;
		*)
			body="$raw"
			;;
	esac
	body="$(echo "$body" | sed 's/::/_/g')"
	# 已是内部逗号格式
	case "$body" in
		*,start=*,stop=*)
			echo "$body"
			return 0
			;;
	esac
	path="$(echo "$body" | awk '{print $1}')"
	start="$(echo "$body" | sed -n 's/.*start=\([^[:space:]]*\).*/\1/p')"
	stop="$(echo "$body" | sed -n 's/.*stop=\([^[:space:]]*\).*/\1/p')"
	[ -n "$path" ] && [ -n "$start" ] && [ -n "$stop" ] || return 1
	echo "${path},start=${start},stop=${stop}"
}

# 读取 config.conf 中用户自定义供电开关（可多行）
zg_load_user_switches() {
	local line entry
	ZG_USER_SWITCHES=""
	[ -f "$CONF" ] || return 0
	local n=0
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			power_switch=*)
				entry="$(zg_normalize_power_switch_entry "$line")" || {
					zg_log_once "psw_bad_$n" warn "忽略无效 power_switch 行"
					continue
				}
				ZG_USER_SWITCHES="$ZG_USER_SWITCHES $entry"
				n=$((n + 1))
				;;
		esac
	done <"$CONF"
	if [ "$n" -gt 0 ]; then
		zg_log_once user_sw debug "已加载 ${n} 条自定义供电开关"
	fi
}

# 按列表写停充/恢复；ok_var 为 stop_ok 或 start_ok
zg_write_switch_list() {
	local mode="$1"
	local list="$2"
	local i route val
	for i in $list; do
		route="$(echo "$i" | sed -n 's/,start=.*//g;$p')"
		[ -f "$route" ] || continue
		if [ "$mode" = "stop" ]; then
			val="$(echo "$i" | sed -n 's/.*,stop=//g;s/_/ /g;$p')"
			zg_write_node "$route" "$val"
			stop_nodes="$stop_nodes $route=$val"
			log_log=1
			stop_ok=1
		else
			val="$(echo "$i" | sed -n 's/.*,start=//g;s/,stop=.*//g;s/_/ /g;$p')"
			zg_write_node "$route" "$val"
			start_node="$route"
			start_val="$val"
			log_log2=1
			start_ok=1
		fi
	done
}

# 组装 switch_list：扫描结果 + 兜底 +（若有）本机 MCA；并加载用户 power_switch
zg_build_switch_list() {
	switch_list="$(cat "$LIST_SWITCH" 2>/dev/null)"
	switch_list="$switch_list $ZG_FALLBACK_SWITCHES"
	zg_load_device_profile
	if [ "$ZG_MCA" = "1" ] && [ -n "$ZG_MCA_PATH" ]; then
		switch_list="$switch_list ${ZG_MCA_PATH},start=${ZG_MCA_START},stop=${ZG_MCA_STOP}"
	fi
	zg_load_user_switches
}

zg_mca_write() {
	local val="$1"
	local label="$2"
	[ "$ZG_MCA" = "1" ] || return 1
	[ -n "$ZG_MCA_PATH" ] && [ -f "$ZG_MCA_PATH" ] || return 1
	zg_write_node "$ZG_MCA_PATH" "$val"
	if [ "$label" = "stop" ]; then
		stop_nodes="$ZG_MCA_PATH=$val (MCA)"
		log_log=1
		stop_ok=1
	else
		start_node="$ZG_MCA_PATH"
		start_val="$val"
		log_log2=1
		start_ok=1
	fi
	return 0
}

# 有 preferred_switch 时优先写该节点；MCA 机型仍优先 MCA
zg_pref_write() {
	local mode="$1"
	local val
	zg_load_device_profile
	[ -n "$ZG_PREF_PATH" ] && [ -f "$ZG_PREF_PATH" ] || return 1
	if [ "$mode" = "stop" ]; then
		val="$ZG_PREF_STOP"
		[ -n "$val" ] || return 1
		zg_write_node "$ZG_PREF_PATH" "$val"
		stop_nodes="$ZG_PREF_PATH=$val (preferred)"
		log_log=1
		stop_ok=1
	else
		val="$ZG_PREF_START"
		[ -n "$val" ] || return 1
		zg_write_node "$ZG_PREF_PATH" "$val"
		start_node="$ZG_PREF_PATH"
		start_val="$val"
		log_log2=1
		start_ok=1
	fi
	return 0
}

zg_power_stop() {
	stop_ok=0
	stop_nodes=""
	if zg_mca_write "$ZG_MCA_STOP" stop; then
		return
	fi
	if zg_pref_write stop; then
		return
	fi
	# 用户自定义供电开关优先于全量扫描
	if [ -n "$ZG_USER_SWITCHES" ]; then
		zg_write_switch_list stop "$ZG_USER_SWITCHES"
		if [ "$stop_ok" = "1" ]; then
			stop_nodes="$stop_nodes (user)"
			return
		fi
	fi
	zg_write_switch_list stop "$switch_list"
}

zg_power_start() {
	start_ok=0
	start_node=""
	start_val=""
	if zg_mca_write "$ZG_MCA_START" start; then
		return
	fi
	if zg_pref_write start; then
		return
	fi
	if [ -n "$ZG_USER_SWITCHES" ]; then
		zg_write_switch_list start "$ZG_USER_SWITCHES"
		if [ "$start_ok" = "1" ]; then
			return
		fi
	fi
	zg_write_switch_list start "$switch_list"
}

zg_power_reset() {
	sleep 2
	zg_power_stop
	sleep 1
	zg_power_start
}

# 卸载时按列表恢复 start 值
zg_restore_switches_from_list() {
	local i route start_val
	[ -f "$LIST_SWITCH" ] || return 1
	for i in $(cat "$LIST_SWITCH"); do
		route="$(echo "$i" | sed -n 's/,start=.*//g;$p')"
		if [ -f "$route" ]; then
			start_val="$(echo "$i" | sed -n 's/.*,start=//g;s/,stop=.*//g;s/_/ /g;$p')"
			echo "$start_val" > "$route" 2>/dev/null
		fi
	done
}

zg_restore_mca_charge() {
	zg_load_device_profile 2>/dev/null || true
	if [ "$ZG_MCA" = "1" ] && [ -n "$ZG_MCA_PATH" ] && [ -f "$ZG_MCA_PATH" ]; then
		chmod 0644 "$ZG_MCA_PATH" 2>/dev/null
		echo "${ZG_MCA_START:-0}" > "$ZG_MCA_PATH" 2>/dev/null
		return 0
	fi
	local mca
	for mca in $ZG_MCA_CANDIDATES; do
		if [ -f "$mca" ]; then
			chmod 0644 "$mca" 2>/dev/null
			echo "0" > "$mca" 2>/dev/null
		fi
	done
}
