#!/system/bin/sh
# charge_guard/loop.sh — 充电阈值守护主循环，由 service.sh 后台拉起
# v1.3：总开关关闭时 10 秒轮询一次（原 60 秒），加快 WebUI 开关响应
CGDIR="${0%/*}"

# 首次运行时生成节点扫描列表 + 设备探测（若缺失）
if [ ! -f "$CGDIR/data/off" ]; then
	if [ ! -f "$CGDIR/data/list_switch" ]; then
		chmod 0755 "$CGDIR/list_switch.sh" 2>/dev/null
		"$CGDIR/list_switch.sh" > /dev/null 2>&1
	fi
	if [ ! -f "$CGDIR/data/device.profile" ]; then
		chmod 0755 "$CGDIR/detect_device.sh" 2>/dev/null
		"$CGDIR/detect_device.sh" > /dev/null 2>&1
	fi
fi

while true; do
	if [ -f "$CGDIR/data/off" ]; then
		# 总开关关闭：10 秒查一次开关状态（v1.3 从 60s 改为 10s）
		sleep 10
		continue
	fi

	"$CGDIR/switch.sh" > /dev/null 2>&1

	if [ -f "$CGDIR/data/is_powered" ]; then
		sleep 3
	else
		sleep 30
	fi
done
