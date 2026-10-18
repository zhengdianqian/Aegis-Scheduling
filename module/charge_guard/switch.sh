#!/system/bin/sh
# charge_guard/switch.sh — 单轮判定：读配置/电量/温度 → 判定 → 写停充/恢复节点
# v1.3：加入停充期自愈（仍在充电→2轮重申→换节点）、status.kv 新增字段
. "${0%/*}/common.sh"

if [ ! -f "$CONF" ]; then
	zg_log_once no_conf error "找不到 charge_guard.conf"
	exit 0
fi
config_conf="$(cat "$CONF" | egrep -v '^#')"

# ── 电量/充电状态/温度：sysfs 直读优先 ──
battery_status_raw="$(cat /sys/class/power_supply/battery/status 2>/dev/null)"
battery_level="$(cat /sys/class/power_supply/battery/capacity 2>/dev/null)"
temperature_raw="$(cat /sys/class/power_supply/battery/temp 2>/dev/null)"
temperature="$(zg_normalize_temperature "$temperature_raw")"

battery_status=""
battery_powered=""
case "$battery_status_raw" in
	Charging) battery_status="2"; battery_powered="powered: true" ;;
	Full) battery_status="5"; battery_powered="powered: true" ;;
	Discharging) battery_status="3" ;;
	"Not charging") battery_status="4" ;;
esac

need_dumpsys=0
[ -z "$battery_level" ] && need_dumpsys=1
[ -z "$battery_status" ] && need_dumpsys=1
[ -z "$temperature" ] && need_dumpsys=1

if [ "$need_dumpsys" = "1" ]; then
	zg_log_once use_dumpsys debug "sysfs 直读不完整，回退 dumpsys battery"
	dumpsys battery > "$DATADIR/.dumpsys_tmp" 2>/dev/null &
	dumpsys_pid=$!
	for wait_i in 1 2 3 4 5; do
		if [ ! -d "/proc/$dumpsys_pid" ]; then break; fi
		sleep 1
	done
	if [ -d "/proc/$dumpsys_pid" ]; then
		zg_log_once dumpsys_to warn "dumpsys battery 超时"
	fi
	kill $dumpsys_pid 2>/dev/null
	dumpsys_battery="$(cat "$DATADIR/.dumpsys_tmp" 2>/dev/null)"
	rm -f "$DATADIR/.dumpsys_tmp"

	[ -z "$battery_level" ] && battery_level="$(zg_dumpsys_level "$dumpsys_battery")"
	if [ -z "$battery_powered" ]; then
		battery_powered="$(echo "$dumpsys_battery" | egrep 'powered: true')"
	fi
	if [ -z "$battery_status" ]; then
		battery_status="$(echo "$dumpsys_battery" | egrep 'status: ' | sed -n 's/.*status: //g;$p')"
	fi
	if [ -z "$temperature" ]; then
		temperature_raw="$(echo "$dumpsys_battery" | egrep 'temperature: ' | sed -n 's/.*temperature: //g;$p')"
		temperature="$(zg_normalize_temperature "$temperature_raw")"
	fi
fi

if [ ! -n "$battery_powered" ]; then
	for usb_online in /sys/class/power_supply/usb/online /sys/class/power_supply/qc_usb/online; do
		if [ -f "$usb_online" ] && [ "$(cat "$usb_online" 2>/dev/null)" = "1" ]; then
			battery_powered="powered: true"
			battery_status="${battery_status:-2}"
			zg_log_once usb_on debug "插电状态来自 $usb_online=1"
			break
		fi
	done
fi

# 供 loop.sh 决定下一轮轮询间隔
if [ -n "$battery_powered" ]; then
	touch "$DATADIR/is_powered"
else
	rm -f "$DATADIR/is_powered"
fi

charge_full="$(echo "$config_conf" | egrep '^charge_full=' | sed -n 's/charge_full=//g;$p')"
power_reset="$(echo "$config_conf" | egrep '^power_reset=' | sed -n 's/power_reset=//g;$p')"
power_stop="$(echo "$config_conf" | egrep '^power_stop=' | sed -n 's/power_stop=//g;$p')"
power_start="$(echo "$config_conf" | egrep '^power_start=' | sed -n 's/power_start=//g;$p')"
temperature_switch="$(echo "$config_conf" | egrep '^temperature_switch=' | sed -n 's/temperature_switch=//g;$p')"
temperature_switch_stop="$(echo "$config_conf" | egrep '^temperature_switch_stop=' | sed -n 's/temperature_switch_stop=//g;$p')"
temperature_switch_start="$(echo "$config_conf" | egrep '^temperature_switch_start=' | sed -n 's/temperature_switch_start=//g;$p')"

_raw_power_stop="$power_stop"
_raw_power_start="$power_start"
_raw_temp_stop="$temperature_switch_stop"
_raw_temp_start="$temperature_switch_start"

# 配置兜底
charge_full="$(zg_clamp_int "$charge_full" 0 1 0)"
power_reset="$(zg_clamp_int "$power_reset" 0 1 0)"
power_stop="$(zg_clamp_level_or_off "$power_stop" 100)"
power_start="$(zg_clamp_int "$power_start" 1 100 95)"
if [ "$power_stop" -le 100 ] 2>/dev/null && [ "$power_stop" -le "$power_start" ] 2>/dev/null; then
	if [ "$power_stop" -gt 5 ] 2>/dev/null; then
		power_start=$((power_stop - 5))
	else
		power_start=1
	fi
fi
temperature_switch="$(zg_clamp_int "$temperature_switch" 0 1 1)"
temperature_switch_stop="$(zg_clamp_int "$temperature_switch_stop" 25 70 60)"
temperature_switch_start="$(zg_clamp_int "$temperature_switch_start" 25 70 50)"
if [ "$temperature_switch_stop" -le "$temperature_switch_start" ] 2>/dev/null; then
	if [ "$temperature_switch_stop" -gt 30 ] 2>/dev/null; then
		temperature_switch_start=$((temperature_switch_stop - 5))
	else
		temperature_switch_start=25
	fi
fi
if [ "$power_stop" != "$_raw_power_stop" ] || [ "$power_start" != "$_raw_power_start" ]; then
	zg_log_once cfg_pwr warn "电量阈值已纠正 ${_raw_power_stop}/${_raw_power_start} → 停充${power_stop}% 恢复${power_start}%"
fi
if [ "$temperature_switch_stop" != "$_raw_temp_stop" ] || [ "$temperature_switch_start" != "$_raw_temp_start" ]; then
	zg_log_once cfg_temp warn "温控阈值已纠正 ${_raw_temp_stop}/${_raw_temp_start} → 停充${temperature_switch_stop}°C 恢复${temperature_switch_start}°C"
fi
off_zg=0

if [ ! -n "$battery_level" ]; then
	for sysfs_cap in /sys/class/power_supply/battery/capacity /sys/class/power_supply/bms/capacity /sys/class/power_supply/battery/soc; do
		if [ -f "$sysfs_cap" ] && [ -r "$sysfs_cap" ]; then
			battery_level="$(zg_safe_cat "$sysfs_cap")"
			[ -n "$battery_level" ] && break
		fi
	done
fi
if [ ! -n "$battery_level" ]; then
	zg_log_once no_batt error "无法获取电池电量"
	exit 0
fi

if [ ! -n "$temperature" ]; then
	for sysfs_temp in /sys/class/power_supply/battery/temp /sys/class/power_supply/bms/temp /sys/class/power_supply/battery/batt_temp; do
		if [ -f "$sysfs_temp" ] && [ -r "$sysfs_temp" ]; then
			temperature_raw="$(zg_safe_cat "$sysfs_temp")"
			temperature="$(zg_normalize_temperature "$temperature_raw")"
			[ -n "$temperature" ] && break
		fi
	done
fi
if [ ! -n "$temperature" ]; then
	zg_log_once no_temp error "无法获取电池温度"
	exit 0
fi

if [ -f "$OFF_FLAG" ]; then
	off_zg=1
	zg_log_once mod_off warn "充电阈值控制已关闭，跳过停充判定"
	power_stop="110"
	power_start="105"
	temperature_switch="0"
	if [ ! -f "$DATADIR/off_d" ]; then
		touch "$DATADIR/off_d"
		rm -f "$DATADIR/power_on" "$DATADIR/power_off"
	fi
else
	rm -f "$DATADIR/off_d"
	zg_log_once_clear mod_off
fi

switch_stop_mode=0
log_log=0
cpu_log=0
log_log2=0
cpu_log2=0
full_log=0
reset_log=0
battery_stop_reason=0

if [ ! -f "$LIST_SWITCH" ]; then
	if [ -f "$BINDIR/list_switch.sh" ]; then
		chmod 0755 "$BINDIR/list_switch.sh"
		"$BINDIR/list_switch.sh" > /dev/null 2>&1
	fi
	exit 0
fi

_zg_switch_list_ready=0
zg_ensure_switch_list() {
	[ "$_zg_switch_list_ready" = "1" ] && return
	zg_build_switch_list
	_zg_switch_list_ready=1
}

zg_charge_full() {
	if [ "$charge_full" = "1" -a "$battery_level" = "100" -a "$power_stop" = "100" ]; then
		now_current="$(zg_safe_cat /sys/class/power_supply/battery/current_now)"
		if [ "$battery_status" = "5" ]; then
			rm -f "$DATADIR/now_c"
		else
			full_log=1
			if [ -n "$now_current" ]; then
				now_current="$(echo "$now_current" | sed -n 's/-//g;$p')"
				if [ "$now_current" -lt "100000" ]; then
					echo "$now_current" >> "$DATADIR/now_c"
				else
					rm -f "$DATADIR/now_c"
				fi
				now_current_n="$(cat "$DATADIR/now_c" | wc -l)"
				if [ "$now_current_n" -ge "3" ]; then
					full_log=0
					rm -f "$DATADIR/now_c"
					zg_log debug "电量$battery_level 充满再停：涓流已结束，即将停充"
				fi
			fi
		fi
	fi
}

# ── v1.3 停充期自愈：标记已停充但实际仍在充电 → 重申 → 换节点 ──
zg_check_leak() {
	[ -f "$DATADIR/power_switch" ] || return 0
	local st
	st="$(cat /sys/class/power_supply/battery/status 2>/dev/null)"
	case "$st" in
		"Not charging"|"Discharging"|"Full")
			rm -f "$DATADIR/leak_count"
			return 0
			;;
	esac
	# 仍在充电：漏充
	local cnt
	cnt="$(cat "$DATADIR/leak_count" 2>/dev/null)"
	cnt="${cnt:-0}"
	cnt=$((cnt + 1))
	echo "$cnt" > "$DATADIR/leak_count"
	zg_log warn "停充后仍在充电（漏充第${cnt}轮）status=$st"
	if [ "$cnt" -lt 2 ]; then
		# 重申：再写一次 stop
		zg_ensure_switch_list
		zg_power_stop
		if [ "$stop_ok" = "1" ]; then
			zg_log info "重申停充成功 [$stop_nodes]"
		fi
	else
		# 连续 2 轮仍漏充：清除 preferred，立即重新扫描停充
		zg_clear_preferred_switch
		rm -f "$DATADIR/leak_count"
		zg_log warn "连续漏充，清除 preferred 重新探测节点"
		zg_ensure_switch_list
		zg_power_stop
		if [ "$stop_ok" = "1" ]; then
			zg_log info "换节点后停充成功 [$stop_nodes]"
		else
			zg_log error "换节点后仍无法停充，本机可能无有效节点"
		fi
	fi
}

battery_status_data=0
if [ "$battery_status" = "2" -o "$battery_status" = "5" ]; then
	battery_status_data=1
fi

if [ -n "$battery_powered" ]; then
	if [ -f "$LOG_FILE" ]; then
		log_n="$(cat "$LOG_FILE" | wc -l)"
		[ "$log_n" -gt "80" ] && sed -i '1,10d' "$LOG_FILE"
	fi
	# 先做漏充自愈检查
	zg_check_leak

	if [ "$temperature_switch" = "1" ]; then
		if [ "$temperature_switch_stop" -gt "$temperature_switch_start" -a "$temperature" -ge "$temperature_switch_stop" ]; then
			touch "$DATADIR/temp_switch"
			cpu_log=1
		fi
	fi
	if [ "$power_stop" -gt "$power_start" -a "$battery_level" -ge "$power_stop" ]; then
		zg_charge_full
		if [ "$full_log" = "0" ]; then
			switch_stop_mode=1
			battery_stop_reason=1
		fi
	fi
	if [ "$switch_stop_mode" = "1" -o "$cpu_log" = "1" ]; then
		first_stop=0
		[ ! -f "$DATADIR/power_switch" ] && first_stop=1
		if [ "$cpu_log" = "0" -a "$charge_full" != "1" -a "$first_stop" = "1" ]; then
			power_stop_time="$(echo "$config_conf" | egrep '^power_stop_time=' | sed -n 's/power_stop_time=//g;$p')"
			power_stop_time="$(zg_clamp_int "$power_stop_time" 1 120 3)"
			if [ "$power_stop_time" -gt "0" ]; then
				zg_log debug "电量$battery_level 延时功能 继续充电${power_stop_time}秒 倒计时中"
				sleep "$power_stop_time"
			fi
		fi
		sleep 3
		zg_ensure_switch_list
		zg_power_stop
		if [ "$stop_ok" = "1" ]; then
			touch "$DATADIR/power_switch"
			[ "$battery_stop_reason" = "1" ] && touch "$DATADIR/battery_switch"
			if [ "$first_stop" = "1" -a "$log_log" = "1" ]; then
				if [ "$cpu_log" = "1" ]; then
					zg_log info "电量$battery_level 触发温控停充 温度$temperature [$stop_nodes]"
				else
					zg_log info "电量$battery_level 停止充电 [$stop_nodes]"
				fi
			fi
		elif [ "$first_stop" = "1" ]; then
			if [ ! -f "$DATADIR/no_node_logged" ]; then
				zg_log error "电量$battery_level 未找到有效充电控制节点！本机可能不支持自动停充"
				touch "$DATADIR/no_node_logged"
			fi
		fi
		if [ -f "$DATADIR/power_switch" -a "$battery_stop_reason" = "1" ]; then
			touch "$DATADIR/battery_switch"
		fi
	else
		reset_log=1
	fi
	if [ ! -f "$DATADIR/power_on" -a "$off_zg" != "1" ]; then
		rm -f "$DATADIR/power_off"
		touch "$DATADIR/power_on"
		if [ "$power_reset" = "1" -a "$reset_log" = "1" ]; then
			zg_power_reset
			zg_log info "电量$battery_level 触发自动拔插功能"
		fi
	fi
else
	if [ ! -f "$DATADIR/power_off" -a "$off_zg" != "1" ]; then
		rm -f "$DATADIR/now_c" "$DATADIR/power_on"
		touch "$DATADIR/power_off"
	fi
fi

if [ -f "$DATADIR/power_switch" ]; then
	temp_ready=1
	battery_ready=1
	if [ -f "$DATADIR/temp_switch" ]; then
		if [ "$temperature_switch" = "1" ] && [ -n "$temperature_switch_start" ] && [ "$temperature" -gt "$temperature_switch_start" ]; then
			temp_ready=0
		else
			cpu_log2=1
		fi
	fi
	if [ -f "$DATADIR/battery_switch" ]; then
		if [ "$power_stop" -le "100" -a "$power_stop" -gt "$power_start" -a "$battery_level" -gt "$power_start" ]; then
			battery_ready=0
		fi
	elif [ "$power_stop" -le "100" -a "$power_stop" -gt "$power_start" -a "$battery_level" -gt "$power_start" ]; then
		battery_ready=0
	fi
	if [ "$temp_ready" = "1" -a "$battery_ready" = "1" ]; then
		sleep 3
		zg_ensure_switch_list
		zg_power_start
		if [ "$start_ok" = "1" ]; then
			rm -f "$DATADIR/power_switch" "$DATADIR/temp_switch" "$DATADIR/battery_switch" "$DATADIR/leak_count"
		fi
		if [ "$log_log2" = "1" ]; then
			if [ "$cpu_log2" = "1" ]; then
				zg_log info "电量$battery_level 触发温控恢复充电 温度$temperature [$start_node <- $start_val]"
			else
				zg_log info "电量$battery_level 恢复充电 [$start_node <- $start_val]"
			fi
		fi
	fi
fi

# ── 写状态快照供 WebUI 读取（v1.3 新增 bat_status/verified/leak/node）──
_cur_st="$(cat /sys/class/power_supply/battery/status 2>/dev/null)"
_verified=0
_leak=0
if [ -f "$DATADIR/power_switch" ]; then
	case "$_cur_st" in
		"Not charging"|"Discharging"|"Full") _verified=1 ;;
		*) _leak=1 ;;
	esac
fi
_active_node=""
if [ -f "$DEVICE_PROFILE" ]; then
	_active_node="$(zg_profile_get preferred_switch 2>/dev/null)"
fi
{
	echo "level=$battery_level"
	echo "temp=$temperature"
	echo "powered=$([ -n "$battery_powered" ] && echo 1 || echo 0)"
	echo "stopped=$([ -f "$DATADIR/power_switch" ] && echo 1 || echo 0)"
	echo "reason_battery=$([ -f "$DATADIR/battery_switch" ] && echo 1 || echo 0)"
	echo "reason_temp=$([ -f "$DATADIR/temp_switch" ] && echo 1 || echo 0)"
	echo "off=$off_zg"
	echo "bat_status=$_cur_st"
	echo "verified=$_verified"
	echo "leak=$_leak"
	echo "node=$_active_node"
	echo "updated_at=$(date +%F_%T)"
} > "$DATADIR/status.kv"
