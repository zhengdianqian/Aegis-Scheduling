#!/system/bin/sh
# ═══════════════════════════════════════════════════════════════════════
#  Aegis 一键适配 SH  ·  在新 ROOT 设备的 MT 管理器里直接执行
#  生成 /sdcard/Aegis_适配建议.txt，包含根据本机硬件计算出的
#  完整 rules.yaml + 完整 config.yaml（所有字段全部填好，可一键复制粘贴）
#
#  设计原则：
#    1. 完全自治——不依赖任何外部工具，纯 POSIX sh + awk
#    2. 兼容 1/2/3/4 簇设备（不仅限 8Gen2 三簇）
#    3. 输出 ready-to-use 的两份完整配置（rules.yaml + config.yaml）
#    4. 同时输出 perf↔频率换算表，方便后续手工微调参考
#    5. 兼容 Toybox 0.8.x（Android 自带 sh/awk）
# ═══════════════════════════════════════════════════════════════════════

OUT=/sdcard/Aegis_适配建议.txt
DATAF=/data/local/tmp/aegis_probe_data.txt

# ────────────────────────────────────────────────────────────────────────
# 1. 硬件探测：扫描所有 cpufreq policy，记录 related_cpus / 频率范围 / capacity
# ────────────────────────────────────────────────────────────────────────
> "$DATAF"

POL_DIRS=$(ls /sys/devices/system/cpu/cpufreq 2>/dev/null | grep '^policy[0-9][0-9]*$' | sort -V)
[ -z "$POL_DIRS" ] && {
    # fallback：直接按 CPU 编号扫
    POL_DIRS=""
    for c in 0 1 2 3 4 5 6 7 8 9; do
        [ -d /sys/devices/system/cpu/cpu$c/cpufreq ] && POL_DIRS="$POL_DIRS cpu$c"
    done
}

NCLUSTER=0
for p in $POL_DIRS; do
    # policy 名（取数字部分作为 ID）
    PID=$(echo "$p" | sed 's/policy//;s/cpu//')
    BASE="/sys/devices/system/cpu/cpufreq/$p"
    [ ! -d "$BASE" ] && BASE="/sys/devices/system/cpu/$p/cpufreq"
    [ ! -d "$BASE" ] && continue

    REL=$(cat "$BASE/related_cpus" 2>/dev/null)
    FMIN=$(cat "$BASE/cpuinfo_min_freq" 2>/dev/null)
    FMAX=$(cat "$BASE/cpuinfo_max_freq" 2>/dev/null)
    [ -z "$FMIN" ] && FMIN=300000
    [ -z "$FMAX" ] && FMAX=2000000

    # 取该簇第一个 CPU 编号（用于读取 per-CPU 的 cpu_capacity）
    if [ -z "$REL" ]; then
        REL="$PID"
    fi
    FIRST_CPU=$(echo "$REL" | awk '{print $1}')
    [ -z "$FIRST_CPU" ] && FIRST_CPU="$PID"

    # capacity 优先读 per-policy 的 cpu_capacity（多数内核没这文件），
    # 回落到 per-CPU 的 /sys/devices/system/cpu/cpuN/cpu_capacity，
    # 最后才默认 1024（无法区分簇能力时）
    CAP=$(cat "$BASE/cpu_capacity" 2>/dev/null)
    if [ -z "$CAP" ] || [ "$CAP" = "0" ]; then
        CAP=$(cat /sys/devices/system/cpu/cpu$FIRST_CPU/cpu_capacity 2>/dev/null)
    fi
    if [ -z "$CAP" ] || [ "$CAP" = "0" ]; then
        # 再扫一遍 related_cpus 里所有 CPU，取第一个非零 capacity
        for c in $REL; do
            tc=$(cat /sys/devices/system/cpu/cpu$c/cpu_capacity 2>/dev/null)
            if [ -n "$tc" ] && [ "$tc" != "0" ]; then
                CAP="$tc"; break
            fi
        done
    fi
    [ -z "$CAP" ] || [ "$CAP" = "0" ] && CAP=1024

    echo "$PID|$REL|$FMIN|$FMAX|$CAP" >> "$DATAF"
    NCLUSTER=$((NCLUSTER + 1))
done

# 找出最小 capacity 簇（作为权重归一化基准）
MINCAP=$(awk -F'|' 'NR==1{m=$5} {if($5<m && $5>0)m=$5} END{print m+0}' "$DATAF")
[ "$MINCAP" -le 0 ] && MINCAP=300

# ────────────────────────────────────────────────────────────────────────
# 2. 计算每簇权重 + perf→频率映射表
#    公式：weight = 1 + √(cap/MINCAP − 1)
#    （与 Aegis 源码 src/scheduler/fas/policy_mgmt.rs 一致）
# ────────────────────────────────────────────────────────────────────────

# 按容量升序排序，方便后续识别"小/中/大/超大"角色
sort -t'|' -k5,5n "$DATAF" -o "$DATAF.sorted"

# ────────────────────────────────────────────────────────────────────────
# 3. 生成主输出文件
# ────────────────────────────────────────────────────────────────────────
{
echo "══════════════════════════════════════════════════════════════════"
echo " Aegis 调度 · 本机适配建议"
echo " 生成时间：$(date '+%Y-%m-%d %H:%M:%S')"
echo "══════════════════════════════════════════════════════════════════"
echo
echo "【一】硬件探测结果"
echo "────────────────────────────────────────────────────────────────────"
echo "检测到 $NCLUSTER 个 CPU 簇（policy）："
echo
printf "%-8s %-14s %-12s %-14s %-10s %-8s\n" "policy" "CPU列表" "最小频率" "最大频率" "capacity" "权重"
echo "────────────────────────────────────────────────────────────────────"

# 同时把权重写到一个临时文件供后续规则生成使用
SORTED_WEIGHTS=/data/local/tmp/aegis_weights.txt
> "$SORTED_WEIGHTS"

awk -F'|' -v mc="$MINCAP" -v wfile="$SORTED_WEIGHTS" '
    BEGIN { idx=0 }
    {
        cap=$5+0; if(cap<=0) cap=mc;
        r=cap/mc-1.0; if(r<0) r=0;
        w=1.0+sqrt(r);
        fmin=$3; fmax=$4;
        # 输出表格行
        printf "%-8s %-14s %-12s %-14s %-10s %.2f\n", "policy"$1, $2, fmin, fmax, cap, w;
        # 保存权重供后续使用：pid|fmin|fmax|cap|weight
        print $1"|"fmin"|"fmax"|"cap"|"w >> wfile;
        idx++;
        # 同时记录最大簇的频率上限（用于 perf_ceil 换算参考）
        if(cap>maxcap){maxcap=cap; maxpid=$1; maxfmax=fmax}
        if(idx==1){minfmax_s=$4} # 最小簇的最高频
    }
    END {
        printf "\n[基准 capacity = %d (最小簇)]\n", mc;
        printf "[最大簇：policy%s, max_freq=%d kHz = %.2f GHz]\n", maxpid, maxfmax, maxfmax/1000000.0;
    }
' "$DATAF.sorted"

echo
echo "【二】perf（归一化性能 0.0~1.0）→ 各簇实际频率换算"
echo "────────────────────────────────────────────────────────────────────"
echo "perf_floor/ceil/init/cold_boot 的语义："
echo "  perf = 0.0  → 每簇取该簇 hardware_min_freq（最深省电底）"
echo "  perf = 1.0  → 每簇取该簇 hardware_max_freq（满频）"
echo "  perf = 0.30 → 小核约 30%×(fmax−fmin)+fmin，依此类推"
echo
echo "参考表（5 个常用 perf 档位 → 各簇 MHz）："
printf "%-8s" "perf"
awk -F'|' '{printf "  %-10s", "policy"$1}' "$DATAF.sorted"
printf "\n"

for P in 0.30 0.50 0.70 0.85 1.00; do
    printf "%-8s" "$P"
    awk -F'|' -v p="$P" '{
        f=$2 + p*($3-$2);
        printf "  %-10.1f", f/1000;
    }' "$DATAF.sorted"
    printf "\n"
done

echo
echo "【三】完整 rules.yaml（复制粘贴即可用）"
echo "────────────────────────────────────────────────────────────────────"
echo "说明：以下内容已经按本机硬件计算好 cluster_profiles 权重。"
echo "      如果你不知道某项怎么改，就保持原样——这是 Aegis 默认推荐值。"
echo
echo "# ────────────────────────────────────────────────────────────────────"
echo "# Aegis 调度规则 · 本机自动适配"
echo "# 生成：$(date '+%Y-%m-%d %H:%M:%S') by Aegis_一键适配.sh"
echo "# 簇数：$NCLUSTER ｜ 最小 capacity：$MINCAP"
echo "# ────────────────────────────────────────────────────────────────────"
echo
echo "# 调度总开关"
echo "aegis_scheduler: true"
echo
echo "# FAS 全局开关："
echo "#   false = 默认。普通应用不走 FAS、帧探针/eBPF 全程休眠（零空转功耗）；"
echo "#           但下方 app_modes 里单独指定为 fas 的游戏，识别到前台仍会唤醒 FAS。"
echo "#   true  = 所有应用都可进入 FAS（一般不建议，仅指定游戏走 FAS）。"
echo "fas_enabled: false"
echo
echo "dynamic_enabled: true"
echo "# 日常全局模式：powersave 省电 / balance 均衡 / performance 性能 / fast 极速 / fas"
echo "global_mode: \"balance\""
echo
echo "app_modes:"
echo "  # 把需要帧感知调度的游戏/相机加在这里，包名自定义即可（系统应用也支持）："
echo "  com.miHoYo.GenshinImpact: \"fas\""
echo "  com.tencent.tmgp.sgame: \"fas\""
echo "  com.tencent.tmgp.speedmobile: \"fas\""
echo "  com.tencent.lolm: \"fas\""
echo "  # 系统相机（预览/录像帧率敏感）"
echo "  com.android.camera: \"fas\""
echo "  # 文本编辑器/阅读器如有频率空高现象可钉到 powersave："
echo "  # bin.mt.plus: \"powersave\""
echo
echo "ignored_apps:"
echo "  - com.android.systemui"
echo
echo "fas_rules:"
echo "  # 全局默认档位（未在 per_app_profiles 中配置的游戏使用）"
echo "  fps_gears: [30.0, 60.0, 90.0, 120.0, 144.0]"
echo "  fps_margin: 3.0"
echo
echo "  per_app_profiles:"
echo "    \"com.miHoYo.GenshinImpact\":"
echo "      target_fps: [30, 60]"
echo "      fps_margin: 4.0"
echo "    \"com.tencent.tmgp.pubgmhd\":"
echo "      target_fps: [60, 90, 120]"
echo "      fps_margin: 3.0"
echo "    \"com.tencent.tmgp.speedmobile\":"
echo "      target_fps: [30, 60, 120, 144]"
echo "      fps_margin: 3.0"
echo "    \"com.tencent.tmgp.sgame\":"
echo "      target_fps: [60, 90, 120]"
echo "      fps_margin: 3.0"
echo "    \"com.hypergryph.arknights\":"
echo "      target_fps: [30, 60]"
echo "    \"com.miHoYo.hkrpg\":"
echo "      target_fps: [30, 60]"
echo "    \"com.kurogame.mingchao\":"
echo "      target_fps: [24, 30, 45, 60]"
echo "    \"com.tencent.lolm\":"
echo "      target_fps: [60, 90, 120, 144]"
echo "      fps_margin: 3.0"
echo
echo "  # PID 控制器核心系数（上游默认，未动）"
echo "  pid:"
echo "    kp: 0.05"
echo "    ki: 0.01"
echo "    kd: 0.006"
echo
echo "  # ── 集群算力感知（本机自动计算）──"
echo "  # 权重公式：1 + √(cap/mincap − 1)，与 Aegis 源码 policy_mgmt.rs 一致"
echo "  # 含义：低 perf 时压制大簇频率省电、高 perf 时保证大簇拉满；"
echo "  #       值越大低载越省电、重负载响应略缓。"
echo "  auto_capacity_weight: false"
echo "  cluster_profiles:"
awk -F'|' '{
    # $1=pid, $2=fmin, $3=fmax, $4=cap, $5=weight(已计算)
    # 重新计算以确保精度
    cap=$4+0; r=cap/'"$MINCAP"'-1; if(r<0)r=0; w=1+sqrt(r);
    # 根据 cap 的大小给个角色标签
    if(cap<400) role="小核";
    else if(cap<700) role="中核";
    else if(cap<900) role="大核";
    else role="超大核";
    printf "    - capacity_weight: %.2f   # policy%s %s cap=%d\n", w, $1, role, cap;
}' "$DATAF.sorted"

echo
echo "  # 归一化性能 (Perf) 范围"
echo "  # perf_floor 0.30 → 游戏起步地板，防进场卡顿"
echo "  # perf_init  0.50 → 初始 perf"
echo "  # perf_cold_boot 0.85 → 冷启动保护"
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
echo "  # 降档紧急提频 (Boost)"
echo "  downgrade_boost_perf_inc: 0.18"
echo "  downgrade_boost_duration: 45"
echo
echo "  # 连续稳态快速衰减"
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
echo "  # 应用切换 / 息屏检测"
echo "  app_switch_gap_ms: 3000.0"
echo "  app_switch_resume_perf: 0.60"
echo
echo "  # 其他底层机制"
echo "  freq_force_reapply_interval: 30"
echo "  fixed_max_frame_ms: 500.0"
echo "  cold_boot_ms: 3500"
echo
echo
echo "【四】完整 config.yaml（复制粘贴即可用）"
echo "────────────────────────────────────────────────────────────────────"
echo "说明：以下 4 模式（powersave/balance/performance/fast）已包含"
echo "      所有 cpu_load_governor 字段。powersave/balance/performance 三模式"
echo "      经过 2026-10-15 v3 调参：headroom 降到 1.05-1.10 + up_threshold 提到"
echo "      0.75-0.85 + down_threshold 提到 0.45-0.55，拓宽中频带，避免频率空转。"
echo
echo "# 元数据信息"
echo "meta:"
echo "  name: \"aegis_config\""
echo "  author: \"Aegis\""
echo "  language: \"zh\""
echo "  loglevel: \"INFO\""
echo
echo "# 主要功能开关"
echo "function:"
echo "  CpuIdleScalingGovernor: false"
echo "  IOOptimization: true"
echo
echo "# IO 设置"
echo "IO_Settings:"
echo "  Scheduler: \"none\""
echo "  read_ahead_kb: \"128\""
echo "  nomerges: \"2\""
echo "  iostats: \"0\""
echo
echo "# CPU 空闲设置"
echo "CpuIdle:"
echo "  current_governor: \"\""
echo
echo "# ────────────────────────────────────────────────────────────────────"
echo "# 性能模式 · 4 档 (powersave/balance/performance/fast)"
echo "# v7 规则：target_perf = raw_util × gain，gain 是随负载单调上升的连续直线"
echo "#   raw ≤ down_threshold → gain = low_load_gain（静态深度压频）"
echo "#   raw ≥ up_threshold   → gain = headroom_factor（重载留帧余量）"
echo "#   两阈值之间线性插值，全程无阶跃。"
echo "# perf 归一化：0.0 = 各簇硬件最低频，1.0 = 各簇最高频"
echo "# ────────────────────────────────────────────────────────────────────"
echo
echo "# 省电模式：v3 拓宽中频带 + 保留 0.70 上限"
echo "#   util 0.55→0.22→1203MHz；util 0.70→0.51→1998MHz；util 0.85→capped 2530MHz"
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
echo "# 标准模式：target 紧跟真实 util，不放大；启动可冲满频，平时不空转"
echo "# 2026-10-15 v3 调参：headroom 1.15→1.05 + up_threshold 0.70→0.85 + down 0.40→0.50"
echo "#   + slow_down 0.50→0.55 + down_fast 0.30→0.25 + spike_jump 0.35→0.50"
echo "#   perf_ceil 保持 1.0（不封顶，3360MHz 可达），low_load_gain 保持 0.55（仅静态生效）"
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
echo "# 性能模式：v3 拓宽中频带，仍倾向高位但不再中载直冲 2700MHz"
echo "#   util 0.45→0.34→1528MHz；util 0.60→0.555→2129MHz；util 0.75→0.825→2876MHz；util 1.0→3360MHz"
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
echo "# 极速模式：锁最高频，不参与下探"
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
echo
echo
echo "【五】移植步骤"
echo "────────────────────────────────────────────────────────────────────"
echo "1. 把上面【三】的内容存为 /data/adb/modules/aegis_power_matrix/rules.yaml"
echo "2. 把上面【四】的内容存为 /data/adb/modules/aegis_power_matrix/config/config.yaml"
echo "3. rules.yaml 改完会原子热重载（无需重启守护）；config.yaml 改完需重启守护："
echo "     su -c 'killall -9 Aegis; nohup /data/adb/modules/aegis_power_matrix/core/bin/Aegis > /dev/null 2>&1 &'"
echo "4. 用 'cat /data/adb/modules/aegis_power_matrix/config/current_mode.txt' 验证当前模式"
echo "5. 切换模式：在 WebUI 里点选，或直接改 rules.yaml 的 global_mode 字段（热重载生效）"
echo
echo
echo "【六】非 8Gen2 设备的特殊说明"
echo "────────────────────────────────────────────────────────────────────"
echo "本机检测到 $NCLUSTER 个簇。处理策略如下："
echo
case "$NCLUSTER" in
    1)
        echo "  · 单簇设备（如某些中低端 SoC 或全部 unified 调度）："
        echo "    cluster_profiles 只列 1 项，权重 1.00；"
        echo "    balance 模式 perf_ceil 0.85 仍生效，会限制在该簇 85% 最高频。"
        ;;
    2)
        echo "  · 双簇设备（如 4×A53 + 4×A72，或 8Gen3 的 prime+cluster 简化模型）："
        echo "    cluster_profiles 列 2 项；最大簇权重按公式自动算；"
        echo "    balance 模式 perf_ceil 0.85 限制最大簇峰值。"
        ;;
    3)
        echo "  · 三簇设备（如 8Gen2/8Gen1 等，小核+中核+超大核）："
        echo "    标准模型，本 SH 输出已完全适用；"
        echo "    8Gen1/8Gen2 容量/频率档位略有不同但权重公式通用。"
        ;;
    4)
        echo "  · 四簇设备（如 8Gen3 的 prime+大+中+小 4 簇架构）："
        echo "    cluster_profiles 列 4 项；每簇权重按公式自动算；"
        echo "    建议把最小簇 perf_floor 提到 0.20，避免最小簇被压到 600MHz 卡顿。"
        ;;
    *)
        echo "  · 检测到 $NCLUSTER 簇（非常规）："
        echo "    cluster_profiles 已按本机实际列出 $NCLUSTER 项；"
        echo "    请人工核对权重是否合理，必要时手工调整 auto_capacity_weight=true 让守护自动算。"
        ;;
esac
echo
echo
echo "【七】关键提醒"
echo "────────────────────────────────────────────────────────────────────"
echo "1. rules.yaml 的 cluster_profiles 顺序应与本机 /sys/devices/system/cpu/cpufreq/"
echo "   policy* 的编号顺序一致（policy0/policy3/policy7 等）。本 SH 输出已按 capacity"
echo "   升序排列，如与 policy 数字顺序不同请人工调换。"
echo
echo "2. 权重来源：内核 /sys/devices/system/cpu/cpu*/cpu_capacity。如果该文件返回 0 或"
echo "   不存在，本 SH 回落到默认 1024，所有权重会变成 1.00（无法区分簇能力）。"
echo "   此时建议改 auto_capacity_weight: true 让守护自己测，或手工填值。"
echo
echo "3. balance 模式 v3 调参不封顶 perf_ceil（保持 1.0），3360MHz 在重载/启动时可达。"
echo "   平时通过 headroom 1.05（基本不放大）+ up_threshold 0.85（仅真重载触发 headroom）"
echo "   实现 target 紧跟真实 util：util 0.5→target≈0.275（1354MHz），util 0.85→2946MHz，util 1.0→3360MHz。"
echo
echo "4. config.yaml 修改后必须重启守护才生效；rules.yaml 改完无需重启（原子热重载）。"
echo
echo "5. 删除该 SH 临时文件：rm /data/local/tmp/aegis_probe_data.txt /data/local/tmp/aegis_weights.txt"
echo
echo "══════════════════════════════════════════════════════════════════"
echo " END"
echo "══════════════════════════════════════════════════════════════════"
} > "$OUT"

# 清理临时文件
rm -f "$DATAF" "$DATAF.sorted" "$SORTED_WEIGHTS" 2>/dev/null

echo ""
echo "============================================================"
echo " 完成！建议文件已生成："
echo "   $OUT"
echo "============================================================"
echo "行数：$(wc -l < "$OUT")"
echo "字节：$(wc -c < "$OUT")"
