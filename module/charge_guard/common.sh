#!/system/bin/sh
# charge_guard/common.sh — 充电阈值守护：路径初始化 + 库加载
# Aegis 电源矩阵 · 充电阈值控制（充电阈值控制，原型移植自开源充电控制方案并重写整合）

MODDIR="${MODDIR:-$(cd "${0%/*}/.." 2>/dev/null && pwd)}"
[ -n "$MODDIR" ] || MODDIR="/data/adb/modules/aegis_power_matrix"

BINDIR="$MODDIR/charge_guard"
LIBDIR="$BINDIR/lib"
DATADIR="$BINDIR/data"
CONF="$MODDIR/charge_guard.conf"
LIST_SWITCH="$DATADIR/list_switch"
LOG_FILE="$DATADIR/log.log"
OFF_FLAG="$DATADIR/off"
DEVICE_PROFILE="$DATADIR/device.profile"

mkdir -p "$DATADIR" 2>/dev/null

# 防止重复 source
if [ -n "$ZG_LIBS_LOADED" ]; then
	return 0 2>/dev/null || exit 0
fi
ZG_LIBS_LOADED=1

if [ ! -d "$LIBDIR" ]; then
	echo "[充电守护] 缺少 charge_guard/lib，模块文件不完整" >&2
	return 1 2>/dev/null || exit 1
fi

. "$LIBDIR/util.sh"
. "$LIBDIR/profile.sh"
. "$LIBDIR/charge.sh"
