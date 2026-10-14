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
    /// 上一 tick 的原始 max_util，用于尖峰跳升检测
    last_util: f32,
    /// 交互态剩余拍数（>0 时处于手势交互窗口）
    hold: u32,
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
                hold: 0,
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

        for cluster in &mut self.clusters {
            let raw_util = cluster.max_util(core_utils);
            let jump = raw_util - cluster.last_util;

            // ── 交互态检测（全部基于压缩前的原始 util）──────────────────────
            // 入场：前台 App 最忙线程（UI/RenderThread）超过 interact_fg_util
            //   且本簇原始负载达到 down_fast_threshold（极低负载线）。
            // 不要求负载"跳变"——低速慢拖的负载是从静态平缓爬升的（0.1→0.35），
            // 若要求 jump 阈值，整个慢拖手势都会停在压缩路径里，正是信息流
            // 慢滑丢帧的来源。前台闸门足以排除 MIUI 后台同步等系统噪声
            // （实测无交互时前台线程利用率仅 0.03~0.05）。
            // 高负载线（raw>=up_threshold）在前台忙时自然包含在入场条件内。
            let fg_active = fg_max_util >= self.cfg.interact_fg_util;
            // 驻留续期：交互态内前台仍在渲染（含松手后的 fling 惯性动画，
            // RenderThread 属于前台进程）且簇负载不低于极低负载线时刷新计时，
            // 覆盖停顿再拖；前台一旦真正空闲，hold 最迟 interact_hold_ticks
            // 后必然退出，杜绝后台噪声让交互态永久锁死。
            let onset = self.cfg.interact_hold_ticks > 0
                && fg_active
                && raw_util >= self.cfg.down_fast_threshold;
            let sustaining = cluster.hold > 0
                && fg_active
                && raw_util >= self.cfg.down_fast_threshold;
            if onset || sustaining {
                cluster.hold = self.cfg.interact_hold_ticks.max(1);
            }
            let interactive = cluster.hold > 0;
            // 本拍判定用当前 hold，判定后倒计时（续期成功会重新填满）
            if cluster.hold > 0 {
                cluster.hold -= 1;
            }
            cluster.last_util = raw_util;

            // ── 有效负载：尖峰抑制只在非交互态生效 ─────────────────────────
            // 旧实现对触摸首拍先做尖峰衰减、再做低载压缩，两次串联后高速
            // 通道阈值永远凑不齐，造成每段滑动开头 2~4 拍的给频延迟（丢帧）。
            // 交互态直接使用原始负载；尖峰抑制仅保留给前台空闲时的后台
            // 孤立噪声尖峰（前台闸门挡住了所有触摸类负载）。
            let util = if !interactive && jump >= self.cfg.spike_jump_threshold {
                raw_util - jump * (1.0 - self.cfg.spike_decay)
            } else {
                raw_util
            };

            // headroom 余量只在交互态给满（帧级突发需要缓冲）；非交互态恒为
            // 1.0——余量是为渲染线程准备的，后台任务（同步/厂商服务）不需要，
            // 给余量只会让静态频率随锯齿虚高。
            let headroom = if interactive {
                self.cfg.headroom_factor
            } else {
                1.0
            };

            // 低/中载目标压缩：非交互态（前台 App 线程不忙）下对所有负载水平
            // 都按增益压缩——100ms 采样能看到 MIUI 后台任务的单拍满载突发，
            // 若只在 up_threshold 以下压缩，这类锯齿会反复把频率顶到高档并
            // 慢速回落，静态功耗虚高；后台任务没有帧延迟约束，中档频即可。
            // 交互态（触摸/滑动/前台渲染）不压缩，保证供给。
            let effective_util = if interactive {
                util
            } else {
                util * self.cfg.low_load_gain
            };

            let target_perf = (effective_util * headroom)
                .clamp(self.cfg.perf_floor, self.cfg.perf_ceil);
            let old_perf = cluster.current_perf;

            if target_perf > old_perf {
                cluster.down_wait = 0;

                if interactive {
                    // 交互态升频：跳过 up_wait 确认；突增首拍（onset）直接
                    // 一步到位，消除采样+确认+平滑三重滞后；后续拍正常平滑
                    cluster.up_wait = 0;
                    let rate = if onset {
                        1.0
                    } else {
                        self.cfg.smoothing_up
                    };
                    cluster.current_perf += (target_perf - old_perf) * rate;
                } else {
                    cluster.up_wait += 1;

                    // 升频速率限制：必须连续 up_rate_limit_ticks 才执行
                    if cluster.up_wait < self.cfg.up_rate_limit_ticks {
                        continue;
                    }

                    let is_high_load = util >= self.cfg.up_threshold;
                    let is_significant_jump = target_perf > old_perf + self.cfg.up_jump_threshold;

                    if is_high_load || is_significant_jump {
                        cluster.current_perf += (target_perf - old_perf) * self.cfg.smoothing_up;
                    } else {
                        // 滞回带内升频：速率随 util 接近 up_threshold 线性提升——
                        // 低 util 端用 slow_up_scale 防抖，高 util 端逼近全速，
                        // 避免中等负载（如 73%）下 0.008/tick 的慢速爬升导致体验卡顿
                        let span = (self.cfg.up_threshold - self.cfg.down_threshold).max(1e-6);
                        let gap = ((util - self.cfg.down_threshold) / span).clamp(0.0, 1.0);
                        let speed = self.cfg.smoothing_up
                            * (self.cfg.slow_up_scale + (1.0 - self.cfg.slow_up_scale) * gap);
                        cluster.current_perf += (target_perf - old_perf) * speed;
                    }
                }
            } else {
                cluster.up_wait = 0;
                cluster.down_wait += 1;

                if interactive {
                    // 交互态降频：手势窗口内只允许匀速平缓回落，
                    // 禁止极低载快速沉底（×4.5），手指停顿/再拖时不付起步延迟
                    cluster.current_perf +=
                        (target_perf - old_perf) * self.cfg.smoothing_down;
                } else if cluster.down_wait >= self.cfg.down_rate_limit_ticks
                    || util < self.cfg.down_fast_threshold
                {
                    // 极低负载立即快速降频（跳过 down_wait 确认期），
                    // 消除尖峰消失后 perf 长时间悬停高位的滞后
                    // 降频门控：只要目标低于当前即可降，避免滞回带内锁死高位
                    let active_smoothing_down = if util < self.cfg.down_fast_threshold {
                        // 极低负载：快速回落
                        self.cfg.smoothing_down * self.cfg.down_fast_mult
                    } else if util < self.cfg.down_threshold {
                        // 跌破降频阈值：正常速率降频
                        self.cfg.smoothing_down
                    } else {
                        // 滞回带内（down_threshold..up_threshold）：慢速下探防抖
                        self.cfg.smoothing_down * self.cfg.slow_down_scale
                    };
                    cluster.current_perf += (target_perf - old_perf) * active_smoothing_down;
                }
            }

            cluster.current_perf = cluster.current_perf.clamp(self.cfg.perf_floor, self.cfg.perf_ceil);
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
