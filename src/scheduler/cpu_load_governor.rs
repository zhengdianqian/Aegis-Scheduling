/*
 * Copyright (C) 2026 yuki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


use crate::scheduler::config::CpuLoadGovernorConfig;
use crate::utils::FastWriter;
use log::{info, debug, warn};
use std::fs;

use crate::i18n::{t, t_with_args};
use crate::fluent_args;

// ════════════════════════════════════════════════════════════════
//  PolicyRestore — CLG 接管前的系统状态快照，release 时恢复
// ════════════════════════════════════════════════════════════════

struct PolicyRestore {
    policy_id: i32,
    /// 读取失败时为 None，恢复时跳过该字段，不写退化值
    governor: Option<String>,
    min_freq: Option<u32>,
    max_freq: Option<u32>,
    /// 该 policy 的硬件最大可用频率，恢复时先放宽上限到它
    hw_max: u32,
}

// ════════════════════════════════════════════════════════════════
//  ClusterState — 单 cluster 运行时状态
// ════════════════════════════════════════════════════════════════

struct ClusterState {
    policy_id: i32,
    affected_cpus: Vec<usize>,
    available_freqs: Vec<u32>,
    cached_ratios: Vec<f32>,
    boost_max: u32,
    max_writer: FastWriter,
    min_writer: FastWriter,
    current_perf: f32,
    current_freq: u32,
    down_wait: u32,
    up_wait: u32,
    /// 上一 tick 的原始 max_util，用于负载突增（jump）检测
    last_util: f32,
}

impl ClusterState {
    fn find_nearest_freq(&self, target_ratio: f32) -> u32 {
        let idx = self.cached_ratios.partition_point(|&r| r < target_ratio);
        if idx == 0 {
            self.available_freqs[0]
        } else if idx >= self.available_freqs.len() {
            *self.available_freqs.last().unwrap()
        } else {
            let lo = idx - 1;
            let hi = idx;
            if (self.cached_ratios[hi] - target_ratio).abs()
                < (self.cached_ratios[lo] - target_ratio).abs()
            { self.available_freqs[hi] } else { self.available_freqs[lo] }
        }
    }

    fn write_freq(&mut self, freq: u32) {
        if freq == self.current_freq { return; }
        let ok = if freq >= self.current_freq {
            // 升频：先拉高 max 再拉高 min
            let ok_max = self.max_writer.write_value_force(freq);
            let ok_min = self.min_writer.write_value_force(freq);
            ok_max && ok_min
        } else {
            // 降频：先降 min 再降 max
            let ok_min = self.min_writer.write_value_force(freq);
            let ok_max = self.max_writer.write_value_force(freq);
            ok_max && ok_min
        };
        // 仅在两端均写入成功时更新缓存，失败则下次 tick 自动重试
        if ok {
            self.current_freq = freq;
        }
    }

    fn max_util(&self, core_utils: &[f32]) -> f32 {
        self.affected_cpus.iter()
            .filter_map(|&cpu| core_utils.get(cpu))
            .copied()
            .fold(0.0_f32, f32::max)
    }
}

// ════════════════════════════════════════════════════════════════
//  CpuLoadGovernor — 主控制器
// ════════════════════════════════════════════════════════════════

pub struct CpuLoadGovernor {
    clusters: Vec<ClusterState>,
    /// CLG 接管前的系统状态，release 时恢复（首次 init 时捕获）
    restore: Vec<PolicyRestore>,
    cfg: CpuLoadGovernorConfig,
    active: bool,
    log_counter: u32,
}

impl CpuLoadGovernor {
    pub fn new() -> Self {
        Self {
            clusters: Vec::new(),
            restore: Vec::new(),
            cfg: CpuLoadGovernorConfig::default(),
            active: false,
            log_counter: 0,
        }
    }

    pub fn is_active(&self) -> bool {
        self.active
    }

    pub fn init_policies(&mut self, gov_cfg: &CpuLoadGovernorConfig) {
        self.release();
        self.cfg = gov_cfg.clone();
        self.normalize_cfg();

        let clusters = crate::scheduler::get_cpu_policies();

        for policy in &clusters {
            let pid = policy.id;
            let gov_path = format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_governor", pid);
            let min_path = format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_min_freq", pid);
            let max_path = format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_max_freq", pid);

            let freq_path = format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_available_frequencies", pid);
            let mut freqs: Vec<u32> = fs::read_to_string(&freq_path)
                .unwrap_or_default()
                .split_whitespace()
                .filter_map(|s| s.parse().ok())
                .collect();
            if freqs.is_empty() { continue; }
            freqs.sort_unstable();
            freqs.dedup();

            // 合并 boost 频率（部分平台额外暴露的高频点），去重排序
            if !policy.boost_frequencies.is_empty() {
                freqs.extend(&policy.boost_frequencies);
                freqs.sort_unstable();
                freqs.dedup();
            }

            let affected = Self::read_affected_cpus(pid);
            if affected.is_empty() { continue; }

            let fmin = *freqs.first().unwrap() as f32;
            let fmax = *freqs.last().unwrap() as f32;
            let range = (fmax - fmin).max(1.0);
            let cached_ratios: Vec<f32> = freqs.iter()
                .map(|&f| (f as f32 - fmin) / range)
                .collect();

            let max_writer = FastWriter::new(format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_max_freq", pid));
            let min_writer = FastWriter::new(format!(
                "/sys/devices/system/cpu/cpufreq/policy{}/scaling_min_freq", pid));

            if !max_writer.is_valid() || !min_writer.is_valid() {
                warn!("{}", t_with_args("clg-writer-invalid", &fluent_args!(
                    "pid" => pid.to_string(),
                    "max_valid" => max_writer.is_valid().to_string(),
                    "min_valid" => min_writer.is_valid().to_string()
                )));
                continue;
            }

            // 记录系统原始状态（每个将被接管的 policy 单独记录），release 时恢复。
            // 必须位于 governor 写入之前，确保 release 能还原所有被接管的 cluster。
            // 读取失败记录为 None：恢复时跳过对应字段，避免写退化值（如 0）。
            let governor = fs::read_to_string(&gov_path).ok()
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty());
            let min_freq = fs::read_to_string(&min_path).ok()
                .and_then(|s| s.trim().parse::<u32>().ok());
            let max_freq = fs::read_to_string(&max_path).ok()
                .and_then(|s| s.trim().parse::<u32>().ok());
            // 同 policy 只保留最新快照（覆盖上次恢复失败遗留的旧记录）
            self.restore.retain(|r| r.policy_id != pid);
            self.restore.push(PolicyRestore {
                policy_id: pid,
                governor,
                min_freq,
                max_freq,
                hw_max: *freqs.last().unwrap(),
            });

            let _ = crate::utils::try_write_file(&gov_path, "performance");

            let init_perf = self.cfg.perf_init.clamp(self.cfg.perf_floor, self.cfg.perf_ceil);
            let boost_max = policy.boost_frequencies.iter().copied().max().unwrap_or(0);
            let mut cluster = ClusterState {
                policy_id: pid,
                affected_cpus: affected.clone(),
                available_freqs: freqs,
                cached_ratios,
                boost_max,
                max_writer,
                min_writer,
                current_perf: init_perf,
                current_freq: 0,
                down_wait: 0,
                up_wait: 0,
                last_util: 0.0,
            };

            let init_freq = cluster.find_nearest_freq(init_perf);
            let init_ok = cluster.max_writer.write_value_force(init_freq)
                && cluster.min_writer.write_value_force(init_freq);
            // 仅两端均写入成功才缓存频率；失败保持 0，下次 tick write_freq 自动重试
            if init_ok {
                cluster.current_freq = init_freq;
            }

            info!("{}", t_with_args("clg-init", &fluent_args!(
                "pid" => pid.to_string(),
                "cpus" => format!("{:?}", affected),
                "fmin" => (fmin / 1000.0).to_string(),
                "fmax" => (fmax / 1000.0).to_string(),
                "perf" => format!("{:.2}", init_perf),
                "freq" => init_freq.to_string()
            )));

            self.clusters.push(cluster);
        }

        self.active = !self.clusters.is_empty();
        if self.active {
            info!("{}", t_with_args("clg-activated", &fluent_args!("count" => self.clusters.len().to_string())));
        } else {
            warn!("{}", t("clg-no-clusters"));
        }
    }

    pub fn release(&mut self) {
        if self.active { info!("{}", t("clg-deactivated")); }
        // 恢复系统原始状态，避免 release 后 CPU 悬停在 CLG 最后写入的值上。
        // 恢复失败的条目保留，下次 release/init 时重试，避免静默漂移。
        self.restore.retain(|r| !Self::restore_policy(r));
        self.clusters.clear();
        self.active = false;
        self.log_counter = 0;
    }

    pub fn reload_config(&mut self, gov_cfg: &CpuLoadGovernorConfig) {
        self.cfg = gov_cfg.clone();
        self.normalize_cfg();
        debug!("{}", t_with_args("clg-config-reloaded", &fluent_args!(
            "up" => format!("{:.2}", self.cfg.up_threshold),
            "down" => format!("{:.2}", self.cfg.down_threshold),
            "floor" => format!("{:.2}", self.cfg.perf_floor),
            "ceil" => format!("{:.2}", self.cfg.perf_ceil)
        )));
    }

    /// 校验并规范化配置：防止 perf_floor > perf_ceil / NaN 导致 f32::clamp panic
    fn normalize_cfg(&mut self) {
        let floor = self.cfg.perf_floor;
        let ceil = self.cfg.perf_ceil;
        if floor.is_finite() && ceil.is_finite() && floor > ceil {
            warn!("{}", t_with_args("clg-perf-clamped", &fluent_args!(
                "floor" => format!("{:.2}", floor),
                "ceil" => format!("{:.2}", ceil)
            )));
        }
        self.cfg.normalize();
    }

    /// 将单个 policy 恢复为接管前的原始状态。
    /// 返回是否全部写入成功：失败时返回 false，调用方保留快照以便重试。
    fn restore_policy(r: &PolicyRestore) -> bool {
        let gov_path = format!(
            "/sys/devices/system/cpu/cpufreq/policy{}/scaling_governor", r.policy_id);
        let min_path = format!(
            "/sys/devices/system/cpu/cpufreq/policy{}/scaling_min_freq", r.policy_id);
        let max_path = format!(
            "/sys/devices/system/cpu/cpufreq/policy{}/scaling_max_freq", r.policy_id);

        let mut all_ok = true;
        // 写序保证任意中间状态均满足 min <= max：
        // 1) 恢复 governor（读取失败为 None 时跳过，保持现状）；
        // 2) 上限先放宽到硬件最大值（恒 >= 当前下限）；
        // 3) 恢复下限；4) 恢复上限。各步失败均记录，由调用方决定重试。
        if let Some(gov) = &r.governor {
            if crate::utils::write_to_file(&gov_path, gov.as_bytes()).is_err() {
                all_ok = false;
            }
        }
        if crate::utils::write_to_file(&max_path, r.hw_max.to_string()).is_err() {
            all_ok = false;
        }
        if let Some(min) = r.min_freq {
            if crate::utils::write_to_file(&min_path, min.to_string()).is_err() {
                all_ok = false;
            }
        }
        if let Some(max) = r.max_freq {
            if crate::utils::write_to_file(&max_path, max.to_string()).is_err() {
                all_ok = false;
            }
        }

        debug!("{}", t_with_args("clg-restore", &fluent_args!(
            "pid" => r.policy_id.to_string(),
            "governor" => r.governor.clone().unwrap_or_else(|| "<unread>".to_string()),
            "min" => r.min_freq.map(|v| v.to_string()).unwrap_or_else(|| "<unread>".to_string()),
            "max" => r.max_freq.map(|v| v.to_string()).unwrap_or_else(|| "<unread>".to_string())
        )));
        all_ok
    }

    pub fn on_load_update(&mut self, core_utils: &[f32], fg_max_util: f32) {
        if !self.active { return; }

        for (idx, cluster) in self.clusters.iter_mut().enumerate() {
            let raw_util = cluster.max_util(core_utils);
            let jump = raw_util - cluster.last_util;
            cluster.last_util = raw_util;

            // 每簇独立上限：cluster_ceil[idx] 覆盖全局 perf_ceil（缺省回退）。
            // normalize 已保证其 ∈ [perf_floor, 1.0]，这里防御性再夹一次。
            let eff_ceil = self.cfg.cluster_ceil.get(idx).copied()
                .filter(|v| v.is_finite())
                .unwrap_or(self.cfg.perf_ceil)
                .clamp(self.cfg.perf_floor, 1.0);
            // 前台进程树（top-app cpuset）整体空闲时，本拍负载来自后台/系统
            // 任务，目标额外封顶 background_ceiling；任何前台线程一旦忙碌
            // （含 WebView/沙箱渲染子进程）本拍即解除。纯函数判定、无驻留。
            let fg_idle = fg_max_util < self.cfg.interact_fg_util;
            let upper = if fg_idle {
                eff_ceil.min(self.cfg.background_ceiling)
            } else {
                eff_ceil
            };
            // ── v7：一条连续增益曲线，无状态、无交互模式、无驻留 ─────────────
            // 频率只跟随真实负载。增益 g 随负载单调线性变化、整条曲线连续：
            //   raw ≤ down_threshold : g = low_load_gain（静态/后台深度压频）
            //   raw ≥ up_threshold   : g = headroom_factor（重载留帧余量）
            //   两者之间线性插值，避免任何阶跃跳频。
            // 实测对照（8Gen2 中核 policy3，499~2803MHz）：
            //   红果播放 raw≈0.42 → perf≈0.24 → 约 1.0GHz（旧交互态误给 1.65G）
            //   MT慢滑 raw≈0.40   → perf≈0.22 → 约 1.0GHz（旧版误冲 2GHz）
            //   设置快滑 raw≈0.85 → perf≈1.0  → 拉满
            let span = (self.cfg.up_threshold - self.cfg.down_threshold).max(1e-6);
            let pos = ((raw_util - self.cfg.down_threshold) / span).clamp(0.0, 1.0);
            let gain = self.cfg.low_load_gain
                + (self.cfg.headroom_factor - self.cfg.low_load_gain) * pos;
            let target_perf = (raw_util * gain)
                .clamp(self.cfg.perf_floor, upper);
            let old_perf = cluster.current_perf;

            if target_perf > old_perf {
                cluster.down_wait = 0;

                // 唯一的加速通道：前台 App 在忙（UI/RenderThread 超闸门）且
                // 本拍负载真实突增 → 跳过连续拍确认，当拍一步到目标。
                // 目标已被增益曲线压过，轻 App 的突增只会快速到达"正确的低档"
                // （MT fling raw 0.6 也只到 ~1.26GHz），不会过给。
                // 后台单拍满载毛刺（同步/厂商服务）前台不忙，必须连续
                // up_rate_limit_ticks 拍确认，单拍毛刺顶不起频率。
                let touch_jump = jump >= self.cfg.spike_jump_threshold
                    && fg_max_util >= self.cfg.interact_fg_util;

                if touch_jump {
                    cluster.up_wait = 0;
                    cluster.current_perf = target_perf;
                } else {
                    cluster.up_wait += 1;

                    // 升频速率限制：必须连续 up_rate_limit_ticks 才执行
                    if cluster.up_wait < self.cfg.up_rate_limit_ticks {
                        continue;
                    }

                    let is_high_load = raw_util >= self.cfg.up_threshold;
                    let is_significant_jump = target_perf > old_perf + self.cfg.up_jump_threshold;

                    if is_high_load || is_significant_jump {
                        cluster.current_perf += (target_perf - old_perf) * self.cfg.smoothing_up;
                    } else {
                        // 滞回带内升频：速率随 util 接近 up_threshold 线性提升——
                        // 低 util 端用 slow_up_scale 防抖，高 util 端逼近全速
                        let speed = self.cfg.smoothing_up
                            * (self.cfg.slow_up_scale + (1.0 - self.cfg.slow_up_scale) * pos);
                        cluster.current_perf += (target_perf - old_perf) * speed;
                    }
                }
            } else {
                cluster.up_wait = 0;
                cluster.down_wait += 1;

                if cluster.down_wait >= self.cfg.down_rate_limit_ticks
                    || raw_util < self.cfg.down_fast_threshold
                {
                    // 低载快速沉底；负载一掉立刻回低频，下一次突增由加速通道
                    // 当拍拉起（≤100ms），升降对称，不需要任何驻留状态。
                    let active_smoothing_down = if raw_util < self.cfg.down_fast_threshold {
                        self.cfg.smoothing_down * self.cfg.down_fast_mult
                    } else if raw_util < self.cfg.down_threshold {
                        self.cfg.smoothing_down
                    } else {
                        // 滞回带内（down_threshold..up_threshold）：慢速下探防抖
                        self.cfg.smoothing_down * self.cfg.slow_down_scale
                    };
                    cluster.current_perf += (target_perf - old_perf) * active_smoothing_down;
                }
            }

            cluster.current_perf = cluster.current_perf.clamp(self.cfg.perf_floor, upper);
            let target_freq = cluster.find_nearest_freq(cluster.current_perf);
            cluster.write_freq(target_freq);
        }

        self.log_counter += 1;
        if self.log_counter % 50 == 0 {
            for c in &self.clusters {
                debug!("{}", t_with_args("clg-tick-log", &fluent_args!(
                    "pid" => c.policy_id.to_string(),
                    "util" => format!("{:.0}", c.max_util(core_utils) * 100.0),
                    "perf" => format!("{:.2}", c.current_perf),
                    "freq" => c.current_freq.to_string(),
                    "boost" => c.boost_max.to_string()
                )));
            }
        }
    }

    fn read_affected_cpus(policy_id: i32) -> Vec<usize> {
        let path = format!(
            "/sys/devices/system/cpu/cpufreq/policy{}/affected_cpus", policy_id);
        fs::read_to_string(&path)
            .unwrap_or_default()
            .split_whitespace()
            .filter_map(|s| s.parse::<usize>().ok())
            .collect()
    }
}
