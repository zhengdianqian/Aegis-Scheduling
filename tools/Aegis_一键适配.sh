#!/system/bin/sh
# Aegis 一键适配
# 在已安装 Aegis 模块的 ROOT 设备上用 MT 管理器直接执行：
#   探测本机硬件 -> 生成 rules.yaml / config.yaml（仅大块作用注释）
#   -> 备份旧配置 -> 覆盖到模块目录 -> 重启守护 -> 输出适配建议 TXT
# 兼容 1/2/3/4 簇设备，纯 POSIX sh + awk，无外部依赖。

OUT=/sdcard/Aegis_适配建议.txt
DATAF=/data/local/tmp/aegis_probe_data.txt
RULES_NEW=/data/local/tmp/aegis_rules_new.yaml
CONFIG_NEW=/data/local/tmp/aegis_config_new.yaml
MODULE_DIR=/data/adb/modules/aegis_power_matrix

[ "$(id -u)" != "0" ] && { echo "请用 ROOT 身份运行（MT 管理器勾选以 Root 执行）"; exit 1; }
[ ! -d "$MODULE_DIR" ] && { echo "错误：未找到模块目录 $MODULE_DIR，请先刷入 Aegis"; exit 1; }

# ── 1. 硬件探测 ──────────────────────────────────────────────────────
: > "$DATAF"
POL_DIRS=$(ls /sys/devices/system/cpu/cpufreq 2>/dev/null | grep '^policy[0-9][0-9]*$' | sort -V)
if [ -z "$POL_DIRS" ]; then
    for c in 0 1 2 3 4 5 6 7 8 9; do
        [ -d /sys/devices/system/cpu/cpu$c/cpufreq ] && POL_DIRS="$POL_DIRS cpu$c"
    done
fi

NCLUSTER=0
for p in $POL_DIRS; do
    PID=$(echo "$p" | sed 's/policy//;s/cpu//')
    BASE="/sys/devices/system/cpu/cpufreq/$p"
    [ ! -d "$BASE" ] && BASE="/sys/devices/system/cpu/$p/cpufreq"
    [ ! -d "$BASE" ] && continue

    REL=$(cat "$BASE/related_cpus" 2>/dev/null)
    FMIN=$(cat "$BASE/cpuinfo_min_freq" 2>/dev/null)
    FMAX=$(cat "$BASE/cpuinfo_max_freq" 2>/dev/null)
    [ -z "$FMIN" ] && FMIN=300000
    [ -z "$FMAX" ] && FMAX=2000000
    [ -z "$REL" ] && REL="$PID"
    FIRST_CPU=$(echo "$REL" | awk '{print $1}')
    [ -z "$FIRST_CPU" ] && FIRST_CPU="$PID"

    CAP=$(cat "$BASE/cpu_capacity" 2>/dev/null)
    if [ -z "$CAP" ] || [ "$CAP" = "0" ]; then
        CAP=$(cat /sys/devices/system/cpu/cpu$FIRST_CPU/cpu_capacity 2>/dev/null)
    fi
    if [ -z "$CAP" ] || [ "$CAP" = "0" ]; then
        for c in $REL; do
            tc=$(cat /sys/devices/system/cpu/cpu$c/cpu_capacity 2>/dev/null)
            if [ -n "$tc" ] && [ "$tc" != "0" ]; then CAP="$tc"; break; fi
        done
    fi
    [ -z "$CAP" ] || [ "$CAP" = "0" ] && CAP=1024

    echo "$PID|$REL|$FMIN|$FMAX|$CAP" >> "$DATAF"
    NCLUSTER=$((NCLUSTER + 1))
done

[ "$NCLUSTER" -lt 1 ] && { echo "错误：未探测到任何 cpufreq policy"; exit 1; }
MINCAP=$(awk -F'|' 'NR==1{m=$5} {if($5<m && $5>0)m=$5} END{print m+0}' "$DATAF")
[ "$MINCAP" -le 0 ] && MINCAP=300
sort -t'|' -k5,5n "$DATAF" -o "$DATAF.sorted"

# ── 2. 生成 rules.yaml（权重按本机 capacity 计算）───────────────────
{
echo "# 调度总开关"
echo "aegis_scheduler: true"
echo "fas_enabled: false"
echo "dynamic_enabled: true"
echo 'global_mode: "balance"'
echo
echo "# 应用单独模式（游戏/相机走 fas）"
echo "app_modes:"
echo '  com.miHoYo.GenshinImpact: "fas"'
echo '  com.tencent.tmgp.sgame: "fas"'
echo '  com.tencent.tmgp.speedmobile: "fas"'
echo '  com.tencent.lolm: "fas"'
echo '  com.android.camera: "fas"'
echo
echo "# 不参与前台识别的应用"
echo "ignored_apps:"
echo "  - com.android.systemui"
echo
echo "# FAS 帧感知调度（仅对 fas 应用生效）"
echo "fas_rules:"
echo "  # 全局帧率档位与余量"
echo "  fps_gears: [30.0, 60.0, 90.0, 120.0, 144.0]"
echo "  fps_margin: 3.0"
echo
echo "  # 单游戏档位"
echo "  per_app_profiles:"
echo '    "com.miHoYo.GenshinImpact":'
echo "      target_fps: [30, 60]"
echo "      fps_margin: 4.0"
echo '    "com.tencent.tmgp.pubgmhd":'
echo "      target_fps: [60, 90, 120]"
echo "      fps_margin: 3.0"
echo '    "com.tencent.tmgp.speedmobile":'
echo "      target_fps: [30, 60, 120, 144]"
echo "      fps_margin: 3.0"
echo '    "com.tencent.tmgp.sgame":'
echo "      target_fps: [60, 90, 120]"
echo "      fps_margin: 3.0"
echo '    "com.hypergryph.arknights":'
echo "      target_fps: [30, 60]"
echo '    "com.miHoYo.hkrpg":'
echo "      target_fps: [30, 60]"
echo '    "com.kurogame.mingchao":'
echo "      target_fps: [24, 30, 45, 60]"
echo '    "com.tencent.lolm":'
echo "      target_fps: [60, 90, 120, 144]"
echo "      fps_margin: 3.0"
echo
echo "  # PID 控制系数"
echo "  pid:"
echo "    kp: 0.05"
echo "    ki: 0.01"
echo "    kd: 0.006"
echo
echo "  # 集群算力权重（按 policy 编号升序）"
echo "  auto_capacity_weight: false"
echo "  cluster_profiles:"
awk -F'|' -v mc="$MINCAP" '{
    cap=$5+0; r=cap/mc-1; if(r<0)r=0; w=1+sqrt(r);
    printf "    - capacity_weight: %.2f\n", w;
}' "$DATAF.sorted"
echo
echo "  # perf 范围（0=各簇最低频，1=各簇最高频）"
echo "  perf_floor: 0.30"
echo "  perf_ceil: 1.0"
echo "  perf_init: 0.50"
echo "  perf_cold_boot: 0.85"
echo "  freq_hysteresis: 0.015"
echo
echo "  # 重帧 / 硬加载"
echo "  heavy_frame_threshold_ms: 150.0"
echo "  loading_cumulative_ms: 2500.0"
echo "  loading_normal_tolerance: 3"
echo "  loading_perf_floor: 0.60"
echo "  loading_perf_ceiling: 0.70"
echo
echo "  # 加载后保护"
echo "  post_loading_ignore_frames: 5"
echo "  post_loading_perf: 0.65"
echo "  post_loading_downgrade_guard: 90"
echo
echo "  # 齿轮切换与阻尼"
echo "  upgrade_confirm_frames: 60"
echo "  downgrade_confirm_frames: 90"
echo "  upgrade_cooldown_after_downgrade: 90"
echo "  gear_dampen_frames: 60"
echo
echo "  # 降档紧急提频"
echo "  downgrade_boost_perf_inc: 0.18"
echo "  downgrade_boost_duration: 45"
echo
echo "  # 稳态快速衰减"
echo "  fast_decay_frame_threshold: 75"
echo "  fast_decay_perf_threshold: 0.70"
echo "  fast_decay_max_step: 0.022"
echo "  fast_decay_min_step: 0.004"
echo
echo "  # Jank 卡顿保护冷却"
echo "  jank_cooldown_frames: 15"
echo
echo "  # PID 输出限幅"
echo "  max_inc_damped: 0.045"
echo "  max_inc_normal: 0.075"
echo "  damped_perf_cap: 0.92"
echo
echo "  # 应用切换 / 息屏"
echo "  app_switch_gap_ms: 3000.0"
echo "  app_switch_resume_perf: 0.60"
echo
echo "  # 其他底层机制"
echo "  freq_force_reapply_interval: 30"
echo "  fixed_max_frame_ms: 500.0"
echo "  cold_boot_ms: 3500"
} > "$RULES_NEW"

# ── 3. 生成 config.yaml（v7 内核 / v3 调参，fast 锁满）─────────────
{
echo "# 元信息"
echo "meta:"
echo '  name: "aegis_config"'
echo '  author: "Aegis"'
echo '  language: "zh"'
echo '  loglevel: "INFO"'
echo
echo "# 功能开关"
echo "function:"
echo "  CpuIdleScalingGovernor: false"
echo "  IOOptimization: true"
echo
echo "# IO 调度设置"
echo "IO_Settings:"
echo '  Scheduler: "none"'
echo '  read_ahead_kb: "128"'
echo '  nomerges: "2"'
echo '  iostats: "0"'
echo
echo "# CPU 空闲设置"
echo "CpuIdle:"
echo '  current_governor: ""'
echo
echo "# 省电模式：静态深度压频，上限 0.70，手势走 jump 通道起步"
echo "powersave:"
echo "  cpu_load_governor:"
echo "    up_threshold: 0.85"
echo "    down_threshold: 0.55"
echo "    smoothing_up: 0.50"
echo "    smoothing_down: 0.45"
echo "    down_rate_limit_ticks: 2"
echo "    up_rate_limit_ticks: 2"
echo "    headroom_factor: 1.05"
echo "    perf_floor: 0.0"
echo "    perf_ceil: 0.70"
echo "    perf_init: 0.25"
echo "    up_jump_threshold: 0.35"
echo "    slow_up_scale: 0.10"
echo "    slow_down_scale: 0.55"
echo "    down_fast_threshold: 0.25"
echo "    down_fast_mult: 4.5"
echo "    spike_jump_threshold: 0.50"
echo "    low_load_gain: 0.40"
echo "    interact_fg_util: 0.25"
echo
echo "# 标准模式：目标紧跟真实负载，不封最高频"
echo "balance:"
echo "  cpu_load_governor:"
echo "    up_threshold: 0.85"
echo "    down_threshold: 0.50"
echo "    smoothing_up: 0.65"
echo "    smoothing_down: 0.35"
echo "    down_rate_limit_ticks: 2"
echo "    up_rate_limit_ticks: 2"
echo "    headroom_factor: 1.05"
echo "    perf_floor: 0.0"
echo "    perf_ceil: 1.0"
echo "    perf_init: 0.35"
echo "    up_jump_threshold: 0.35"
echo "    slow_up_scale: 0.15"
echo "    slow_down_scale: 0.55"
echo "    down_fast_threshold: 0.25"
echo "    down_fast_mult: 4.0"
echo "    spike_jump_threshold: 0.50"
echo "    low_load_gain: 0.55"
echo "    interact_fg_util: 0.25"
echo
echo "# 性能模式：低门槛快升缓降，保留 0.10 底频"
echo "performance:"
echo "  cpu_load_governor:"
echo "    up_threshold: 0.75"
echo "    down_threshold: 0.45"
echo "    smoothing_up: 0.85"
echo "    smoothing_down: 0.20"
echo "    down_rate_limit_ticks: 4"
echo "    up_rate_limit_ticks: 1"
echo "    headroom_factor: 1.10"
echo "    perf_floor: 0.10"
echo "    perf_ceil: 1.0"
echo "    perf_init: 0.55"
echo "    up_jump_threshold: 0.35"
echo "    slow_up_scale: 0.15"
echo "    slow_down_scale: 0.40"
echo "    down_fast_threshold: 0.20"
echo "    down_fast_mult: 3.0"
echo "    spike_jump_threshold: 0.35"
echo "    low_load_gain: 0.75"
echo "    interact_fg_util: 0.25"
echo
echo "# 极速模式：锁满频，不参与调频"
echo "fast:"
echo "  cpu_load_governor:"
echo "    up_threshold: 0.01"
echo "    down_threshold: 0.01"
echo "    smoothing_up: 1.0"
echo "    smoothing_down: 0.01"
echo "    down_rate_limit_ticks: 10"
echo "    up_rate_limit_ticks: 1"
echo "    headroom_factor: 2.0"
echo "    perf_floor: 1.0"
echo "    perf_ceil: 1.0"
echo "    perf_init: 1.0"
echo "    up_jump_threshold: 0.35"
echo "    slow_up_scale: 0.02"
echo "    slow_down_scale: 0.5"
echo "    down_fast_threshold: 0.10"
echo "    down_fast_mult: 2.5"
echo "    spike_jump_threshold: 0.35"
echo "    low_load_gain: 1.0"
echo "    interact_fg_util: 0.25"
} > "$CONFIG_NEW"

# ── 4. 生成文件校验（非空 + 关键字段齐全，块头注释允许保留）────────
for f in "$RULES_NEW" "$CONFIG_NEW"; do
    [ -s "$f" ] || { echo "错误：$f 生成失败"; exit 1; }
done
grep -q '^aegis_scheduler:' "$RULES_NEW" || { echo "错误：rules.yaml 字段缺失"; exit 1; }
grep -q '^fas_rules:' "$RULES_NEW" || { echo "错误：rules.yaml 缺 fas_rules"; exit 1; }
for m in powersave balance performance fast; do
    grep -q "^$m:" "$CONFIG_NEW" || { echo "错误：config.yaml 缺 $m 模式"; exit 1; }
done
NW=$(grep -c 'capacity_weight' "$RULES_NEW")
[ "$NW" = "$NCLUSTER" ] || { echo "错误：cluster_profiles 条数($NW) != 簇数($NCLUSTER)"; exit 1; }

# ── 5. 备份 + 部署 + 重启守护 ───────────────────────────────────────
TS=$(date +%Y%m%d_%H%M%S)
cp "$MODULE_DIR/rules.yaml" "$MODULE_DIR/rules.yaml.bak.$TS" 2>/dev/null
cp "$MODULE_DIR/config/config.yaml" "$MODULE_DIR/config/config.yaml.bak.$TS" 2>/dev/null

cp "$RULES_NEW" "$MODULE_DIR/rules.yaml"
chown 0:0 "$MODULE_DIR/rules.yaml"; chmod 644 "$MODULE_DIR/rules.yaml"
cp "$CONFIG_NEW" "$MODULE_DIR/config/config.yaml"
chown 0:0 "$MODULE_DIR/config/config.yaml"; chmod 644 "$MODULE_DIR/config/config.yaml"

killall -9 Aegis 2>/dev/null
sleep 1
cd /
nohup "$MODULE_DIR/core/bin/Aegis" > /dev/null 2>&1 &
sleep 2
NEWPID=$(pidof Aegis)
[ -n "$NEWPID" ] && DEPLOY_OK=1 || DEPLOY_OK=0

# ── 6. 适配建议 TXT ─────────────────────────────────────────────────
{
echo "══════════════════════════════════════════════════════════════════"
echo " Aegis 调度 · 本机适配建议  $(date '+%Y-%m-%d %H:%M:%S')"
echo "══════════════════════════════════════════════════════════════════"
echo
echo "【一】硬件探测：$NCLUSTER 簇，基准 capacity=$MINCAP（最小簇）"
echo "────────────────────────────────────────────────────────────────────"
printf "%-8s %-14s %-12s %-14s %-10s %-8s\n" "policy" "CPU列表" "最小频率" "最大频率" "capacity" "权重"
awk -F'|' -v mc="$MINCAP" '
    {cap=$5+0; if(cap<=0)cap=mc; r=cap/mc-1; if(r<0)r=0; w=1+sqrt(r);
     printf "%-8s %-14s %-12s %-14s %-10s %.2f\n", "policy"$1, $2, $3, $4, cap, w}
' "$DATAF.sorted"
echo
echo "【二】perf → 各簇频率参考（perf 0.0=最低频，1.0=最高频）"
echo "────────────────────────────────────────────────────────────────────"
printf "%-8s" "perf"
awk -F'|' '{printf "  %-10s", "policy"$1}' "$DATAF.sorted"; printf "\n"
for P in 0.30 0.50 0.70 0.85 1.00; do
    printf "%-8s" "$P"
    awk -F'|' -v p="$P" '{f=$3 + p*($4-$3); printf "  %-10.1f", f/1000}' "$DATAF.sorted"
    printf "\n"
done
echo
echo "【三】已部署 rules.yaml（备份后缀 .bak.$TS）"
echo "────────────────────────────────────────────────────────────────────"
cat "$RULES_NEW"
echo
echo "【四】已部署 config.yaml（v7 内核 / 省电-标准-性能 v3 调参 / fast 锁满）"
echo "────────────────────────────────────────────────────────────────────"
cat "$CONFIG_NEW"
echo
echo "【五】部署结果"
echo "────────────────────────────────────────────────────────────────────"
if [ "$DEPLOY_OK" = "1" ]; then
    echo "成功：rules.yaml / config.yaml 已覆盖，守护已重启（pid=$NEWPID）"
else
    echo "警告：配置已覆盖但未探测到 Aegis 进程，请重启手机或手动重启模块"
fi
echo "回滚：把模块目录内 .bak.$TS 改回原名后重启守护"
echo "当前模式：cat $MODULE_DIR/config/current_mode.txt"
echo
echo "【六】换设备核对项"
echo "────────────────────────────────────────────────────────────────────"
echo "1. cluster_profiles 按 capacity 升序写入（$NW 条）；若与 policy 编号"
echo "   顺序不一致（极少数 SoC），需手工调换条目顺序。"
echo "2. cpu_capacity 读不到时全部回落 1024（权重全 1.00），此时可改"
echo "   auto_capacity_weight: true 让守护自测。"
echo "3. 阈值/增益/jump 类参数与 SoC 无关已直接沿用；只有频率相关项需要"
echo "   按硬件换算，本脚本均已处理。"
echo "══════════════════════════════════════════════════════════════════"
} > "$OUT"

rm -f "$DATAF" "$DATAF.sorted" "$RULES_NEW" "$CONFIG_NEW" 2>/dev/null

echo "============================================================"
if [ "$DEPLOY_OK" = "1" ]; then echo " 适配+部署完成，守护 pid=$NEWPID"; else echo " 配置已部署，守护未起来（建议重启）"; fi
echo " 建议报告：$OUT"
echo "============================================================"
