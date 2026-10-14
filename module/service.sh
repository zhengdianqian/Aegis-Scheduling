#!/system/bin/sh
#
# Aegis 模块启动脚本 (service.sh) — yumi 调度核心，守护进程二进制名 Aegis
#

# 1. 等待系统启动完成
until [ "$(getprop sys.boot_completed)" = "1" ]; do
  sleep 1
done

# 2. 定义路径
[ -z "$MODDIR" ] && MODDIR=${0%/*}

DAEMON_PATH="$MODDIR/core/bin/Aegis"
SCRIPTS_DIR="$MODDIR/scripts"
LOG_DIR="$MODDIR/logs"
LOG_FILE="$LOG_DIR/service.log"

# 确保日志目录存在
mkdir -p "$LOG_DIR"

# 3. 清理旧进程（Aegis 守护名 + 兼容清理早期 yumi 构建，防止双守护并存）
killall -9 Aegis > /dev/null 2>&1
killall -9 yumi  > /dev/null 2>&1

# 4. 设置权限
chmod 755 "$DAEMON_PATH"
if [ -d "$SCRIPTS_DIR" ]; then
  chmod -R 755 "$SCRIPTS_DIR"
fi

# 5. 启动 Aegis 调度守护进程（生产模式，无额外 I/O）
nohup "$DAEMON_PATH" > /dev/null 2>&1 &
