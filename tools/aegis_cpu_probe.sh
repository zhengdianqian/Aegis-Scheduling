#!/system/bin/sh
# ════════════════════════════════════════════════════════════
# Aegis 处理器硬件信息探测脚本（通用，任何安卓 arm64 设备可跑）
# 用途：抓取 cpufreq 频率表 / cpu_capacity 算力 / policy 分簇，
#       作为 config.yaml 与 rules.yaml 三簇定制的原始数据。
# 用法（需 root，电脑连 adb）：
#   adb push aegis_cpu_probe.sh /data/local/tmp/
#   adb shell "su -c sh /data/local/tmp/aegis_cpu_probe.sh"
#   adb pull /data/local/tmp/aegis_cpu_probe.txt
# 或手机终端里：su -c sh /sdcard/aegis_cpu_probe.sh
# ════════════════════════════════════════════════════════════
OUT=/data/local/tmp/aegis_cpu_probe.txt
exec > "$OUT" 2>&1

echo "################ 基础信息 ################"
echo "时间: $(date)"
getprop ro.product.model | sed 's/^/机型: /'
getprop ro.board.platform | sed 's/^/平台: /'
getprop ro.product.cpu.abi | sed 's/^/ABI: /'
getprop ro.build.version.release | sed 's/^/安卓: /'
echo ""

echo "################ 逐核信息 ################"
for c in $(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V); do
    n=${c##*cpu}
    cap=$(cat $c/cpu_capacity 2>/dev/null)
    online=$(cat $c/online 2>/dev/null)
    cur=$(cat $c/cpufreq/scaling_cur_freq 2>/dev/null)
    echo "cpu$n  capacity=$cap  online=$online  cur_freq=$cur"
done
echo ""

echo "################ 分簇 (policy) 频率表 ################"
# policy 目录即一个独立调频簇
for p in $(ls -d /sys/devices/system/cpu/cpufreq/policy* 2>/dev/null | sort -V); do
    id=$(basename $p | sed 's/policy//')
    echo "---------- policy$id ----------"
    echo -n "关联核心: "; cat $p/related_cpus 2>/dev/null
    echo -n "硬件最低 kHz: "; cat $p/cpuinfo_min_freq 2>/dev/null
    echo -n "硬件最高 kHz: "; cat $p/cpuinfo_max_freq 2>/dev/null
    echo -n "当前 governor: "; cat $p/scaling_governor 2>/dev/null
    echo "可用频点 kHz（升序）:"
    cat $p/scaling_available_frequencies 2>/dev/null | tr ' ' '\n' | sort -n | sed 's/^/    /'
    # boost 频点（部分平台有隐藏高频）
    echo -n "boost 频点: "; cat $p/scaling_boost_frequencies 2>/dev/null
    echo ""
done

echo "################ 当前实时频率快照（采样5次，间隔1秒）################"
i=0
while [ $i -lt 5 ]; do
    line=""
    for c in $(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V); do
        n=${c##*cpu}
        f=$(cat $c/cpufreq/scaling_cur_freq 2>/dev/null)
        line="$line cpu$n=$f"
    done
    echo "T+${i}s:$line"
    i=$((i+1))
    sleep 1
done
echo ""

echo "################ 当前各核利用率快照 ################"
head -1 /proc/stat
i=0
while [ $i -lt 3 ]; do
    grep -E '^cpu[0-9]' /proc/stat
    echo "  --"
    sleep 1
    i=$((i+1))
done
echo ""

echo "################ 温控/能效节点（若有）################"
ls /sys/class/thermal/ 2>/dev/null | head -20
echo ""
echo "探测完成，结果文件: $OUT"
