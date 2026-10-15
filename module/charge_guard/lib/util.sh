#!/system/bin/sh
# 通用小工具：超时读节点、温度归一化、调试步进、配置钳位

zg_debug_step() {
	echo "$(date +%F_%T) step$1" >> "$DATADIR/debug.log"
}

# 避免个别 sysfs 读阻塞拖死主循环
zg_safe_cat() {
	cat "$1" > "$DATADIR/.safe_tmp" 2>/dev/null &
	local _pid=$!
	local _i
	for _i in 1 2; do
		if [ ! -d "/proc/$_pid" ]; then break; fi
		sleep 1
	done
	kill $_pid 2>/dev/null
	cat "$DATADIR/.safe_tmp" 2>/dev/null
}

# dumpsys/sysfs 温度统一到摄氏度整数
zg_normalize_temperature() {
	local raw digits normalized
	raw="$(echo "$1" | tr -d ' \r\n')"
	case "$raw" in ""|"-"|*[!0-9-]*) return 1 ;; esac
	digits="${raw#-}"
	case "$digits" in ""|*[!0-9]*) return 1 ;; esac
	if [ "$digits" -ge 10000 ]; then
		normalized=$((raw / 1000))
	elif [ "$digits" -ge 1000 ]; then
		normalized=$((raw / 100))
	elif [ "$digits" -ge 100 ]; then
		normalized=$((raw / 10))
	else
		normalized="$raw"
	fi
	[ "$normalized" -ge -20 -a "$normalized" -le 100 ] || return 1
	echo "$normalized"
}

# dumpsys battery 取电量：仅匹配行首 level:，取首条（适配 Android 16 多字段）
zg_dumpsys_level() {
	echo "$1" | awk '/^[[:space:]]*level:[[:space:]]+/ { print $2; exit }'
}

# 整数钳位：zg_clamp_int 值 最小 最大 默认
zg_clamp_int() {
	local v="$1" lo="$2" hi="$3" def="$4"
	case "$v" in
		""|*[!0-9-]*) echo "$def"; return 0 ;;
	esac
	# 拒绝过大位数（防天文数字拖垮算术）
	case "${v#-}" in
		???????????????*) echo "$def"; return 0 ;;
	esac
	if [ "$v" -lt "$lo" ] 2>/dev/null; then
		echo "$lo"
	elif [ "$v" -gt "$hi" ] 2>/dev/null; then
		echo "$hi"
	else
		echo "$v"
	fi
}

# 电量阈值：1–100 或 110=关闭
zg_clamp_level_or_off() {
	local v="$1" def="${2:-110}"
	case "$v" in
		""|*[!0-9]*) echo "$def"; return 0 ;;
	esac
	if [ "$v" = "110" ]; then
		echo 110
	elif [ "$v" -ge 1 ] 2>/dev/null && [ "$v" -le 100 ] 2>/dev/null; then
		echo "$v"
	else
		echo "$def"
	fi
}

# 运行日志：YYYY-MM-DD_HH:MM:SS [LEVEL] 内容
# LEVEL: info | warn | error | debug
zg_log() {
	_zg_log_write "$@" >>"$LOG_FILE"
}

# 覆盖写入（重建 log.log）
zg_log_new() {
	_zg_log_write "$@" >"$LOG_FILE"
}

# 仅当 KEY 对应内容变化时写入，避免 3 秒主循环刷屏
zg_log_once() {
	_zg_okey="$1"
	_zg_olvl="$2"
	shift 2
	_zg_omsg="$*"
	_zg_of="$DATADIR/.log_once_${_zg_okey}"
	mkdir -p "$DATADIR" 2>/dev/null
	if [ -f "$_zg_of" ] && [ "$(cat "$_zg_of" 2>/dev/null)" = "$_zg_omsg" ]; then
		return 0
	fi
	printf '%s\n' "$_zg_omsg" >"$_zg_of"
	zg_log "$_zg_olvl" "$_zg_omsg"
}

zg_log_once_clear() {
	rm -f "$DATADIR/.log_once_$1"
}

_zg_log_write() {
	local lvl="${1:-info}"
	shift
	case "$lvl" in
		info|INFO) lvl=INFO ;;
		warn|WARN) lvl=WARN ;;
		error|ERROR) lvl=ERROR ;;
		debug|DEBUG) lvl=DEBUG ;;
		*) lvl=INFO ;;
	esac
	echo "$(date +%F_%T) [$lvl] $*"
}

# 兼容旧名
_debug_step() { zg_debug_step "$@"; }
_safe_cat() { zg_safe_cat "$@"; }
_normalize_temperature() { zg_normalize_temperature "$@"; }
