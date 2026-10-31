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

use crate::{
    common::DaemonEvent,
    monitor::{app_detect, sampling::runtime_delta},
    utils::get_ktime_ns,
};
use anyhow::{Context, Result};
use aya::{
    Ebpf,
    maps::{HashMap as BpfHashMap, MapData, PerCpuArray},
    programs::TracePoint,
};
use log::{debug, info};
use std::{
    collections::HashMap,
    sync::{mpsc::Sender, Arc, Mutex},
    time::{Duration, Instant},
};

/// 亮屏采样周期：调度要跟手，保持 100ms（与 CLG 的 tick 语义一致）。
pub const SAMPLE_MS_SCREEN_ON: u64 = 100;
/// 息屏采样周期：后台负载变化缓慢，500ms 足够把频率压到最低档，
/// 同时把 eBPF map 读取 + top-app 扫描 + 消息投递的次数降到 1/5，
/// 守护自身在息屏时的 CPU 唤醒开销同步下降（官方调度的"无感"关键）。
pub const SAMPLE_MS_SCREEN_OFF: u64 = 500;

/// 采样周期只由屏幕状态决定，纯函数便于单测。
pub fn cpu_sample_interval(screen_on: bool) -> Duration {
    Duration::from_millis(if screen_on { SAMPLE_MS_SCREEN_ON } else { SAMPLE_MS_SCREEN_OFF })
}

pub async fn start_cpu_loop(tx: Sender<DaemonEvent>, screen_state: Arc<Mutex<bool>>) -> Result<()> {
    // The CPU ELF contains no FPS maps. Owned maps and the program are dropped
    // together on failure, without leaked Ebpf objects or detached sampler tasks.
    let mut bpf = Ebpf::load(aya::include_bytes_aligned!(concat!(
        env!("OUT_DIR"),
        "/cpu.ebpf"
    )))
    .context("load CPU eBPF object")?;
    let program: &mut TracePoint = bpf
        .program_mut("handle_sched_switch")
        .context("missing handle_sched_switch")?
        .try_into()?;
    program.load().context("load sched_switch program")?;
    program
        .attach("sched", "sched_switch")
        .context("attach sched_switch")?;
    let idle: PerCpuArray<_, u64> = bpf
        .take_map("CORE_IDLE_TIME")
        .context("CORE_IDLE_TIME")?
        .try_into()?;
    let busy: PerCpuArray<_, u64> = bpf
        .take_map("CORE_BUSY_TIME")
        .context("CORE_BUSY_TIME")?
        .try_into()?;
    let switched: PerCpuArray<_, u64> = bpf
        .take_map("CORE_LAST_TIME")
        .context("CORE_LAST_TIME")?
        .try_into()?;
    let current_tid: PerCpuArray<_, u32> = bpf
        .take_map("CORE_CURRENT_TID")
        .context("CORE_CURRENT_TID")?
        .try_into()?;
    let threads: BpfHashMap<MapData, u32, u64> = bpf
        .take_map("THREAD_RUN_TIME")
        .context("THREAD_RUN_TIME")?
        .try_into()?;

    let mut last_cores: HashMap<u32, (u64, u64)> = HashMap::new();
    let mut last_threads: HashMap<u32, u64> = HashMap::new();
    let mut last_pid = 0;
    let mut last_time = get_ktime_ns();
    let mut count = 0u32;
    info!("[CPU Monitor] CPU eBPF monitor initialized");
    loop {
        // 自适应采样：亮屏 100ms / 息屏 500ms。采样间隔由屏幕状态决定，
        // 与调度线程的超时保持同一节拍（亮屏 100ms、息屏 500ms）。
        let screen_on = *screen_state.lock().unwrap_or_else(|e| e.into_inner());
        tokio::time::sleep(cpu_sample_interval(screen_on)).await;
        let sampled_at = Instant::now();
        let cpus = aya::util::online_cpus()
            .map_err(|(_, e)| e)
            .context("read online CPUs")?;
        anyhow::ensure!(!cpus.is_empty(), "no online CPUs");
        // Failed reads must stop publishing healthy-looking zero-load samples.
        let idle_values = idle.get(&0, 0).context("read CORE_IDLE_TIME")?;
        let busy_values = busy.get(&0, 0).context("read CORE_BUSY_TIME")?;
        let switch_values = switched.get(&0, 0).context("read CORE_LAST_TIME")?;
        let tid_values = current_tid.get(&0, 0).context("read CORE_CURRENT_TID")?;
        let now = get_ktime_ns();
        let elapsed = now.saturating_sub(last_time);
        last_time = now;
        if elapsed == 0 {
            continue;
        }
        // The event contract indexes by CPU ID, including holes/offline CPUs.
        let mut core_utils = vec![0.0; *cpus.iter().max().unwrap() as usize + 1];
        let mut pending_by_tid: HashMap<u32, u64> = HashMap::new();
        let mut initialized = false;
        for &cpu in &cpus {
            let index = cpu as usize;
            let raw_idle = *idle_values.get(index).context("missing idle CPU slot")?;
            let raw_busy = *busy_values.get(index).context("missing busy CPU slot")?;
            let last_switch = *switch_values
                .get(index)
                .context("missing switch CPU slot")?;
            let tid = *tid_values.get(index).context("missing TID CPU slot")?;
            let pending = if last_switch > 0 {
                now.saturating_sub(last_switch)
            } else {
                0
            };
            let adjusted = if tid == 0 {
                (raw_idle.saturating_add(pending), raw_busy)
            } else {
                *pending_by_tid.entry(tid).or_default() += pending;
                (raw_idle, raw_busy.saturating_add(pending))
            };
            if let Some(previous) = last_cores.insert(cpu, adjusted) {
                let idle_delta = adjusted.0.saturating_sub(previous.0);
                let busy_delta = adjusted.1.saturating_sub(previous.1);
                let total = idle_delta.saturating_add(busy_delta);
                if total > 0 && last_switch > 0 {
                    core_utils[index] = (busy_delta as f32 / total as f32).clamp(0.0, 1.0);
                    initialized = true;
                }
            }
        }
        last_cores.retain(|cpu, _| cpus.contains(cpu));
        let pid = app_detect::get_current_pid().max(0) as u32;
        // Foreground attribution uses the kernel-maintained top-app cpuset as
        // the authoritative signal: ActivityManager places the foreground
        // app's ENTIRE process tree there (main process, WebView/sandboxed
        // renderer children, mini-program processes, plus system_server input
        // dispatch). This covers every app generically — no package names —
        // and a full read costs ~0.1ms/tick. An empty set means nobody is
        // top-app (e.g. screen off) → foreground util naturally reads 0.
        // Only fall back to scanning /proc/<pid>/task when the node is absent
        // (non-standard ROMs).
        let top_tids = read_topapp_tids();
        let mut next_threads = HashMap::new();
        let mut foreground_max_util = 0.0f32;

        // Difference full per-tid snapshots, including pending runtime on
        // both sides of the sample. The scheduler expects the busiest
        // foreground THREAD, not a sum of unrelated tasks.
        let measure_tid = |tid: u32,
                               previous: Option<u64>,
                               next: &mut HashMap<u32, u64>,
                               max_util: &mut f32|
         -> std::result::Result<(), aya::maps::MapError> {
            let raw = match threads.get(&tid, 0) {
                Ok(value) => value,
                Err(aya::maps::MapError::KeyNotFound) => 0,
                Err(error) => return Err(error),
            };
            let current =
                raw.saturating_add(pending_by_tid.get(&tid).copied().unwrap_or(0));
            let delta = runtime_delta(previous, current);
            *max_util = (*max_util).max((delta as f32 / elapsed as f32).clamp(0.0, 1.0));
            next.insert(tid, current);
            Ok(())
        };

        match &top_tids {
            Some(tids) => {
                // Membership diff doubles as baseline invalidation: a tid that
                // left top-app is dropped; on re-entry its first delta is 0
                // (no previous baseline), so no stale-baseline spike.
                for &tid in tids {
                    measure_tid(
                        tid,
                        last_threads.get(&tid).copied(),
                        &mut next_threads,
                        &mut foreground_max_util,
                    )
                    .context("read THREAD_RUN_TIME")?;
                }
            }
            None => {
                if pid != last_pid {
                    last_threads.clear();
                    last_pid = pid;
                }
                if pid > 0 {
                    if let Ok(entries) = std::fs::read_dir(format!("/proc/{pid}/task")) {
                        for entry in entries.flatten() {
                            let Some(tid) = entry
                                .file_name()
                                .to_str()
                                .and_then(|s| s.parse::<u32>().ok())
                            else {
                                continue;
                            };
                            measure_tid(
                                tid,
                                last_threads.get(&tid).copied(),
                                &mut next_threads,
                                &mut foreground_max_util,
                            )
                            .context("read THREAD_RUN_TIME")?;
                        }
                    }
                }
            }
        }
        last_threads = next_threads;
        if !initialized {
            continue;
        }
        count = count.wrapping_add(1);
        if count % 50 == 0 {
            debug!(
                "[CPU Monitor] cores={core_utils:?}, pid={pid}, foreground={foreground_max_util:.3}"
            );
        }
        if tx
            .send(DaemonEvent::SystemLoadUpdate {
                core_utils,
                foreground_max_util,
                sampled_at,
            })
            .is_err()
        {
            return Ok(());
        }
    }
}

/// Read the kernel-maintained top-app cpuset membership.
/// `None`  = node unavailable (caller falls back to /proc/<pid>/task);
/// `Some([])` = read OK but no foreground top-app (foreground util = 0).
fn read_topapp_tids() -> Option<Vec<u32>> {
    let content = std::fs::read_to_string("/dev/cpuset/top-app/tasks").ok()?;
    Some(
        content
            .split_whitespace()
            .filter_map(|s| s.parse::<u32>().ok())
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_interval_follows_screen_state() {
        assert_eq!(cpu_sample_interval(true), Duration::from_millis(100));
        assert_eq!(cpu_sample_interval(false), Duration::from_millis(500));
    }
}
