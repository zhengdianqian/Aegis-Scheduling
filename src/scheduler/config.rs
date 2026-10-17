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

use serde::Deserialize;

#[derive(Debug, Deserialize, Default)]
pub struct Meta {
    #[serde(default = "default_loglevel", alias = "Loglevel")]
    pub loglevel: String,
    
    #[serde(default = "default_language", alias = "Language")]
    pub language: String,
}

fn default_loglevel() -> String { "INFO".to_string() }
fn default_language() -> String { "en".to_string() }

// ════════════════════════════════════════════════════════════════
//  CPU Load Governor 配置
// ════════════════════════════════════════════════════════════════

#[derive(Debug, Deserialize, Clone)]
pub struct CpuLoadGovernorConfig {
    #[serde(default = "crate::utils::default_true")] pub enabled: bool,
    #[serde(default = "d_clg_up_thresh")] pub up_threshold: f32,
    #[serde(default = "d_clg_down_thresh")] pub down_threshold: f32,
    #[serde(default = "d_clg_smooth_up")] pub smoothing_up: f32,
    #[serde(default = "d_clg_smooth_down")] pub smoothing_down: f32,
    #[serde(default = "d_clg_down_rate")] pub down_rate_limit_ticks: u32,
    #[serde(default = "d_clg_up_rate")] pub up_rate_limit_ticks: u32,
    /// 增益曲线高端（raw ≥ up_threshold 时的增益）：重载留帧余量。
    /// 整条增益曲线在 low_load_gain（低端）与此值（高端）之间随负载线性变化。
    #[serde(default = "d_clg_headroom")] pub headroom_factor: f32,
    #[serde(default = "d_clg_floor")] pub perf_floor: f32,
    #[serde(default = "d_clg_ceil")] pub perf_ceil: f32,
    #[serde(default = "d_clg_init")] pub perf_init: f32,
    /// 升频快速通道判定：target_perf 超过 current_perf 的幅度大于此值时直接快速升频
    #[serde(default = "d_clg_up_jump")] pub up_jump_threshold: f32,
    /// 低负载升频（负载未达 up_threshold 时）对 smoothing_up 的缩放系数
    #[serde(default = "d_clg_slow_up_scale")] pub slow_up_scale: f32,
    /// 滞回带内（down_threshold..up_threshold）降频时对 smoothing_down 的缩放系数，
    /// 用于防抖并避免高频锁定
    #[serde(default = "d_clg_slow_down_scale")] pub slow_down_scale: f32,
    /// 极低负载阈值：util 低于此值触发快速降频
    #[serde(default = "d_clg_down_fast_thresh")] pub down_fast_threshold: f32,
    /// 快速降频时对 smoothing_down 的放大倍数
    #[serde(default = "d_clg_down_fast_mult")] pub down_fast_mult: f32,
    /// 负载突增门槛：单 tick 内簇 max_util 跳升 ≥ 此值，且前台最忙线程超
    /// interact_fg_util 时，升频跳过连续拍确认、当拍到达目标（目标仍按增益
    /// 曲线由真实负载决定，不会过给）。前台不忙的后台毛刺不享受此通道。
    #[serde(default = "d_clg_spike_jump")] pub spike_jump_threshold: f32,
    /// 增益曲线低端（raw ≤ down_threshold 时的增益）：静态/轻载场景（小说、
    /// 短视频、轻量列表）深度压频。增益从此值随负载线性升到 headroom_factor。
    #[serde(default = "d_clg_low_gain")] pub low_load_gain: f32,
    /// 突增免确认的前台闸门：仅当最忙前台 App 线程利用率（UI/RenderThread）
    /// 超过此值时，负载突增才被认定为真实交互并触发当拍升频。用于排除系统
    /// 后台突发（同步/厂商服务）。0 = 不校验前台。
    #[serde(default = "d_clg_interact_fg")] pub interact_fg_util: f32,
    /// 前台空闲压频上限：当 top-app cpuset 内没有任何线程利用率超过
    /// interact_fg_util 时（即用户可见的 App 进程树整体空闲），本拍目标
    /// perf 额外封顶到此值。用于压制后台/厂商服务在中核、超大核上的无效
    /// 抬频（静态阅读时的 MIUI churn、手指离开后的渲染余波）。每拍独立
    /// 计算、无状态无驻留；前台线程一旦忙碌（含 WebView 渲染子进程）
    /// 本拍立即解除。1.0 = 不限制（默认，行为同旧版）。
    #[serde(default = "d_clg_bg_ceil")] pub background_ceiling: f32,
    /// 按簇序号（policy id 升序，0=小核、1=中核、2=超大核）逐簇覆盖
    /// perf_ceil。缺省或越位的簇回退 perf_ceil；空表 = 不覆盖。
    /// 用于单独压低超大核过给（如 [1.0, 1.0, 0.65]），不影响中小核。
    #[serde(default)] pub cluster_ceil: Vec<f32>,
}

fn d_clg_interact_fg() -> f32 { 0.25 }
fn d_clg_bg_ceil() -> f32 { 1.0 }

fn d_clg_up_thresh() -> f32 { 0.80 }
fn d_clg_down_thresh() -> f32 { 0.50 }
fn d_clg_smooth_up() -> f32 { 0.60 }
fn d_clg_smooth_down() -> f32 { 0.30 }
fn d_clg_down_rate() -> u32 { 3 }
fn d_clg_up_rate() -> u32 { 2 }
fn d_clg_headroom() -> f32 { 1.25 }
fn d_clg_floor() -> f32 { 0.15 }
fn d_clg_ceil() -> f32 { 1.0 }
fn d_clg_init() -> f32 { 0.50 }
fn d_clg_up_jump() -> f32 { 0.35 }
fn d_clg_slow_up_scale() -> f32 { 0.02 }
fn d_clg_slow_down_scale() -> f32 { 0.5 }
fn d_clg_down_fast_thresh() -> f32 { 0.10 }
fn d_clg_down_fast_mult() -> f32 { 2.5 }
fn d_clg_spike_jump() -> f32 { 0.35 }
fn d_clg_low_gain() -> f32 { 1.0 }

impl Default for CpuLoadGovernorConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            up_threshold: d_clg_up_thresh(),
            down_threshold: d_clg_down_thresh(),
            smoothing_up: d_clg_smooth_up(),
            smoothing_down: d_clg_smooth_down(),
            down_rate_limit_ticks: d_clg_down_rate(),
            up_rate_limit_ticks: d_clg_up_rate(),
            headroom_factor: d_clg_headroom(),
            perf_floor: d_clg_floor(),
            perf_ceil: d_clg_ceil(),
            perf_init: d_clg_init(),
            up_jump_threshold: d_clg_up_jump(),
            slow_up_scale: d_clg_slow_up_scale(),
            slow_down_scale: d_clg_slow_down_scale(),
            down_fast_threshold: d_clg_down_fast_thresh(),
            down_fast_mult: d_clg_down_fast_mult(),
            spike_jump_threshold: d_clg_spike_jump(),
            low_load_gain: d_clg_low_gain(),
            interact_fg_util: d_clg_interact_fg(),
            background_ceiling: d_clg_bg_ceil(),
            cluster_ceil: Vec::new(),
        }
    }
}

impl CpuLoadGovernorConfig {
    /// 校验并规范化配置：
    /// - 非有限值（NaN/±Inf，如 YAML 溢出值）回退默认，防止污染控制链
    /// - 阈值/系数限制在合理区间
    /// - floor/ceil/init 交叉约束，保证 f32::clamp 永不 panic
    pub fn normalize(&mut self) {
        if !self.up_threshold.is_finite() { self.up_threshold = d_clg_up_thresh(); }
        if !self.down_threshold.is_finite() { self.down_threshold = d_clg_down_thresh(); }
        if !self.smoothing_up.is_finite() { self.smoothing_up = d_clg_smooth_up(); }
        if !self.smoothing_down.is_finite() { self.smoothing_down = d_clg_smooth_down(); }
        if !self.headroom_factor.is_finite() { self.headroom_factor = d_clg_headroom(); }
        if !self.perf_floor.is_finite() { self.perf_floor = d_clg_floor(); }
        if !self.perf_ceil.is_finite() { self.perf_ceil = d_clg_ceil(); }
        if !self.perf_init.is_finite() { self.perf_init = d_clg_init(); }
        if !self.up_jump_threshold.is_finite() { self.up_jump_threshold = d_clg_up_jump(); }
        if !self.slow_up_scale.is_finite() { self.slow_up_scale = d_clg_slow_up_scale(); }
        if !self.slow_down_scale.is_finite() { self.slow_down_scale = d_clg_slow_down_scale(); }
        if !self.down_fast_threshold.is_finite() { self.down_fast_threshold = d_clg_down_fast_thresh(); }
        if !self.down_fast_mult.is_finite() { self.down_fast_mult = d_clg_down_fast_mult(); }
        if !self.spike_jump_threshold.is_finite() { self.spike_jump_threshold = d_clg_spike_jump(); }
        if !self.low_load_gain.is_finite() { self.low_load_gain = d_clg_low_gain(); }
        if !self.background_ceiling.is_finite() { self.background_ceiling = d_clg_bg_ceil(); }

        // 区间限制（语义约束）
        self.up_threshold = self.up_threshold.clamp(0.0, 1.0);
        self.down_threshold = self.down_threshold.clamp(0.0, 1.0);
        // 滞回语义：降频阈值不得高于升频阈值
        if self.down_threshold > self.up_threshold {
            self.down_threshold = self.up_threshold;
        }
        self.smoothing_up = self.smoothing_up.clamp(0.0, 1.0);
        self.smoothing_down = self.smoothing_down.clamp(0.0, 1.0);
        self.slow_up_scale = self.slow_up_scale.clamp(0.0, 1.0);
        self.slow_down_scale = self.slow_down_scale.clamp(0.0, 1.0);
        self.up_jump_threshold = self.up_jump_threshold.clamp(0.0, 1.0);
        self.down_fast_threshold = self.down_fast_threshold.clamp(0.0, 1.0);
        self.spike_jump_threshold = self.spike_jump_threshold.clamp(0.0, 1.0);
        // 低负载增益语义：(0.1..=1.0]，1.0 = 跟随原始负载（不压频）
        self.low_load_gain = self.low_load_gain.clamp(0.1, 1.0);
        if !self.interact_fg_util.is_finite() { self.interact_fg_util = d_clg_interact_fg(); }
        self.interact_fg_util = self.interact_fg_util.clamp(0.0, 1.0);
        // headroom 语义 >= 1（余量放大），down_fast_mult 语义 >= 1（放大）
        self.headroom_factor = self.headroom_factor.clamp(1.0, 3.0);
        self.down_fast_mult = self.down_fast_mult.clamp(1.0, 10.0);

        // 交叉约束（顺序保证 clamp 边界合法）
        if self.perf_floor > self.perf_ceil {
            self.perf_floor = self.perf_ceil;
        }
        self.perf_floor = self.perf_floor.clamp(0.0, 1.0);
        self.perf_ceil = self.perf_ceil.clamp(0.0, 1.0);
        if self.perf_floor > self.perf_ceil {
            self.perf_floor = self.perf_ceil;
        }
        // 背景压频上限：不低于 floor、不高于全局 ceil
        self.background_ceiling = self.background_ceiling
            .clamp(0.0, 1.0)
            .max(self.perf_floor)
            .min(self.perf_ceil);
        // 每簇独立上限：非有限值回退全局 ceil，其余夹到 [floor, 1.0]
        for c in self.cluster_ceil.iter_mut() {
            if !c.is_finite() { *c = self.perf_ceil; }
            *c = c.clamp(0.0, 1.0).max(self.perf_floor);
        }
        self.perf_init = self.perf_init.clamp(self.perf_floor, self.perf_ceil);
    }
}

// ════════════════════════════════════════════════════════════════
//  核心模式与杂项配置
// ════════════════════════════════════════════════════════════════

#[derive(Debug, Deserialize, Default, Clone)]
pub struct Mode {
    #[serde(default, alias = "CpuLoadGovernor")]
    pub cpu_load_governor: CpuLoadGovernorConfig,
}

#[derive(Debug, Deserialize, Clone)]
pub struct IOSettings {
    #[serde(default, rename = "Scheduler")] pub scheduler: String,
    #[serde(default = "default_read_ahead_kb")] pub read_ahead_kb: String,
    #[serde(default = "default_nomerges")] pub nomerges: String,
    #[serde(default = "default_iostats")] pub iostats: String,
}

impl Default for IOSettings {
    fn default() -> Self {
        Self {
            scheduler: String::new(),
            read_ahead_kb: default_read_ahead_kb(),
            nomerges: default_nomerges(),
            iostats: default_iostats(),
        }
    }
}

fn default_read_ahead_kb() -> String { "128".to_string() }
fn default_nomerges() -> String { "2".to_string() }
fn default_iostats() -> String { "0".to_string() }

#[derive(Debug, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub struct CpuIdle {
    pub current_governor: String,
}

#[derive(Debug, Deserialize, Default)]
pub struct FunctionToggles {
    #[serde(rename = "CpuIdleScalingGovernor")] pub cpu_idle_scaling_governor: bool,
    #[serde(rename = "IOOptimization")] pub io_optimization: bool,
}

#[derive(Debug, Deserialize, Default)]
pub struct Config {
    #[serde(default, alias = "Meta")]
    pub meta: Meta,
    #[serde(default)]
    pub function: FunctionToggles,
    #[serde(default, rename = "IO_Settings")]
    pub io_settings: IOSettings,
    #[serde(default, rename = "CpuIdle")]
    pub cpu_idle: CpuIdle,
    
    // 按场景划分的性能模式
    #[serde(default)] pub powersave: Mode,
    #[serde(default)] pub balance: Mode,
    #[serde(default)] pub performance: Mode,
    #[serde(default)] pub fast: Mode,
}

impl Config {
    pub fn from_file(path: &str) -> anyhow::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let config: Config = serde_yaml::from_str(&content)?;
        Ok(config)
    }

    pub fn get_mode(&self, mode_name: &str) -> Option<&Mode> {
        match mode_name {
            "powersave" => Some(&self.powersave),
            "balance" => Some(&self.balance),
            "performance" => Some(&self.performance),
            "fast" => Some(&self.fast),
            _ => None,
        }
    }
}