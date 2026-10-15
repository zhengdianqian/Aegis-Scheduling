#!/system/bin/sh
# 本机充电能力探测 → data/device.profile（按节点存在动态启用，不绑机型名）

ZG_MCA_CANDIDATES="\
/sys/devices/platform/soc/soc:mca_business_charger/handle_state \
/sys/devices/platform/soc/soc:mca_charger/handle_state \
/sys/devices/platform/soc/soc@0:mca_business_charger/handle_state \
/sys/devices/platform/soc/soc@0:mca_charger/handle_state \
/sys/devices/platform/soc/mca_business_charger/handle_state \
/sys/devices/platform/soc/mca_charger/handle_state \
/sys/class/power_supply/mca-charger/handle_state \
/sys/class/power_supply/mca_charger/handle_state \
/sys/class/power_supply/mca-battery/handle_state \
/sys/class/power_supply/mca_battery/handle_state"

zg_find_mca_path() {
	local path
	for path in $ZG_MCA_CANDIDATES; do
		if [ -f "$path" ]; then
			echo "$path"
			return 0
		fi
	done
	path="$(find /sys/devices/platform/ -maxdepth 8 -type f -name 'handle_state' \( -path '*mca*' -o -path '*charg*' \) 2>/dev/null | head -n 1)"
	if [ -n "$path" ] && [ -f "$path" ]; then
		echo "$path"
		return 0
	fi
	return 1
}

zg_write_device_profile() {
	local mca_path="$1"
	local mca=0
	local reassert=0
	local pref_path pref_start pref_stop pref_at
	mkdir -p "$DATADIR" 2>/dev/null
	# 重新探测 MCA 时保留已测出的首选开关
	pref_path="$(zg_profile_get preferred_switch 2>/dev/null)"
	pref_start="$(zg_profile_get preferred_start 2>/dev/null)"
	pref_stop="$(zg_profile_get preferred_stop 2>/dev/null)"
	pref_at="$(zg_profile_get preferred_tested_at 2>/dev/null)"
	if [ -n "$mca_path" ] && [ -f "$mca_path" ]; then
		mca=1
		reassert=1
	else
		mca_path=""
	fi
	# 已测出首选开关时也保持重申
	if [ -n "$pref_path" ]; then
		reassert=1
	fi
	cat > "$DEVICE_PROFILE" << EOF
# 充电守护 device.profile — 由本机节点探测生成，勿手改除非清楚含义
mca=$mca
mca_path=$mca_path
mca_stop=1
mca_start=0
reassert=$reassert
preferred_switch=$pref_path
preferred_start=$pref_start
preferred_stop=$pref_stop
preferred_tested_at=$pref_at
model=$(getprop ro.product.model 2>/dev/null)
device=$(getprop ro.product.device 2>/dev/null)
marketname=$(getprop ro.product.marketname 2>/dev/null)
board=$(getprop ro.product.board 2>/dev/null)
platform=$(getprop ro.board.platform 2>/dev/null)
detected_at=$(date +%F_%T)
EOF
}

# stdout 摘要；返回 0=有 MCA，1=无
zg_detect_and_write_profile() {
	local mca_path=""
	mkdir -p "$DATADIR" 2>/dev/null
	mca_path="$(zg_find_mca_path)" || mca_path=""
	zg_write_device_profile "$mca_path"
	if [ -n "$mca_path" ]; then
		echo "MCA=1 path=$mca_path"
		return 0
	fi
	echo "MCA=0 （未发现 handle_state，使用通用停充节点）"
	return 1
}

zg_profile_get() {
	local key="$1"
	[ -f "$DEVICE_PROFILE" ] || return 1
	sed -n "s/^${key}=//p" "$DEVICE_PROFILE" | head -n 1 | tr -d '\r'
}

zg_profile_set_key() {
	local key="$1"
	local val="$2"
	local tmp
	[ -f "$DEVICE_PROFILE" ] || zg_write_device_profile ""
	tmp="$DEVICE_PROFILE.tmp.$$"
	if grep -q "^${key}=" "$DEVICE_PROFILE" 2>/dev/null; then
		sed "s|^${key}=.*|${key}=${val}|" "$DEVICE_PROFILE" >"$tmp" && mv -f "$tmp" "$DEVICE_PROFILE"
	else
		echo "${key}=${val}" >>"$DEVICE_PROFILE"
	fi
	chmod 0644 "$DEVICE_PROFILE" 2>/dev/null
}

zg_set_preferred_switch() {
	local path="$1"
	local start="$2"
	local stop="$3"
	mkdir -p "$DATADIR" 2>/dev/null
	[ -f "$DEVICE_PROFILE" ] || zg_detect_and_write_profile >/dev/null 2>&1 || true
	zg_profile_set_key preferred_switch "$path"
	zg_profile_set_key preferred_start "$start"
	zg_profile_set_key preferred_stop "$stop"
	zg_profile_set_key preferred_tested_at "$(date +%F_%T)"
	# 有实测首选时启用重申
	zg_profile_set_key reassert 1
}

zg_clear_preferred_switch() {
	[ -f "$DEVICE_PROFILE" ] || return 0
	zg_profile_set_key preferred_switch ""
	zg_profile_set_key preferred_start ""
	zg_profile_set_key preferred_stop ""
	zg_profile_set_key preferred_tested_at ""
}

# 加载到 ZG_MCA / ZG_MCA_PATH / ZG_MCA_STOP / ZG_MCA_START / ZG_REASSERT
# 以及 ZG_PREF_PATH / ZG_PREF_START / ZG_PREF_STOP
zg_load_device_profile() {
	ZG_MCA=0
	ZG_MCA_PATH=""
	ZG_MCA_STOP=1
	ZG_MCA_START=0
	ZG_REASSERT=0
	ZG_PREF_PATH=""
	ZG_PREF_START=""
	ZG_PREF_STOP=""
	if [ ! -f "$DEVICE_PROFILE" ]; then
		zg_detect_and_write_profile >/dev/null 2>&1 || true
	fi
	[ -f "$DEVICE_PROFILE" ] || return 1
	ZG_MCA="$(zg_profile_get mca)"
	ZG_MCA_PATH="$(zg_profile_get mca_path)"
	ZG_MCA_STOP="$(zg_profile_get mca_stop)"
	ZG_MCA_START="$(zg_profile_get mca_start)"
	ZG_REASSERT="$(zg_profile_get reassert)"
	ZG_PREF_PATH="$(zg_profile_get preferred_switch)"
	ZG_PREF_START="$(zg_profile_get preferred_start)"
	ZG_PREF_STOP="$(zg_profile_get preferred_stop)"
	case "$ZG_MCA" in 1) ;; *) ZG_MCA=0 ;; esac
	case "$ZG_REASSERT" in 1) ;; *) ZG_REASSERT=0 ;; esac
	[ -n "$ZG_MCA_STOP" ] || ZG_MCA_STOP=1
	[ -n "$ZG_MCA_START" ] || ZG_MCA_START=0
	if [ -n "$ZG_PREF_PATH" ] && [ ! -f "$ZG_PREF_PATH" ]; then
		ZG_PREF_PATH=""
		ZG_PREF_START=""
		ZG_PREF_STOP=""
	fi
	if [ "$ZG_MCA" = "1" ] && [ -n "$ZG_MCA_PATH" ] && [ ! -f "$ZG_MCA_PATH" ]; then
		zg_detect_and_write_profile >/dev/null 2>&1 || true
		ZG_MCA="$(zg_profile_get mca)"
		ZG_MCA_PATH="$(zg_profile_get mca_path)"
		ZG_REASSERT="$(zg_profile_get reassert)"
		ZG_PREF_PATH="$(zg_profile_get preferred_switch)"
		ZG_PREF_START="$(zg_profile_get preferred_start)"
		ZG_PREF_STOP="$(zg_profile_get preferred_stop)"
		case "$ZG_MCA" in 1) ;; *) ZG_MCA=0 ;; esac
		case "$ZG_REASSERT" in 1) ;; *) ZG_REASSERT=0 ;; esac
		if [ -n "$ZG_PREF_PATH" ] && [ ! -f "$ZG_PREF_PATH" ]; then
			ZG_PREF_PATH=""
			ZG_PREF_START=""
			ZG_PREF_STOP=""
		fi
	fi
	return 0
}
