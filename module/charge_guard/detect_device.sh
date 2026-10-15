#!/system/bin/sh
# 按本机 sysfs 探测充电控制能力，写入 data/device.profile
. "${0%/*}/common.sh"
mkdir -p "$DATADIR" 2>/dev/null
summary="$(zg_detect_and_write_profile)"
zg_log debug "机型探测 $summary"
echo "[充电守护] 设备探测完成: $summary"
echo "[充电守护] profile: $DEVICE_PROFILE"
if [ -f "$DEVICE_PROFILE" ]; then
	cat "$DEVICE_PROFILE"
fi
