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

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use log::{info, warn, debug};
use inotify::{EventMask, Inotify, WatchMask};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering, AtomicI32};
use std::error::Error;
use std::process::Command;
use std::sync::mpsc::Sender;

use crate::common::DaemonEvent;
use crate::i18n::{t, t_with_args};
use crate::fluent_args;
use crate::utils;
use super::config::{self, RulesConfig};

// 缓存有效的 Cgroup 路径索引，避免每次循环都去探测无效路径
static VALID_CGROUP_IDX: AtomicUsize = AtomicUsize::new(usize::MAX);

static CURRENT_PID: AtomicI32 = AtomicI32::new(0);

// Aegis: FAS gate. Only true while the screen is on, fas_enabled is set and
// the foreground app is actually in "fas" mode. The FPS/eBPF monitor sleeps
// (no probes loaded, no perf buffers, no uprobe) whenever this is false.
static FAS_ACTIVE: AtomicBool = AtomicBool::new(false);

pub fn is_fas_active() -> bool {
    FAS_ACTIVE.load(Ordering::Relaxed)
}

// 获取系统已启用的输入法列表
fn get_system_ime_packages() -> HashSet<String> {
    let mut imes = HashSet::new();
    
    let output = Command::new("settings")
        .arg("get")
        .arg("secure")
        .arg("enabled_input_methods")
        .output();

    if let Ok(out) = output {
        let stdout = String::from_utf8_lossy(&out.stdout);
        for entry in stdout.split(':') {
            if let Some(pkg) = entry.split('/').next() {
                let clean_pkg = pkg.trim();
                if !clean_pkg.is_empty() {
                    imes.insert(clean_pkg.to_string());
                    debug!("{}", t_with_args("app-detect-ime-auto", &fluent_args!("pkg" => clean_pkg)));
                }
            }
        }
    }
    
    if imes.is_empty() {
        warn!("{}", t("app-detect-ime-fallback"));
        imes.insert("com.sohu.inputmethod.sogou.xiaomi".to_string());
        imes.insert("com.sohu.inputmethod.sogouoem".to_string());
        imes.insert("com.google.android.inputmethod.latin".to_string());
        imes.insert("com.baidu.input_mi".to_string());
        imes.insert("com.iflytek.inputmethod.miui".to_string());
    }
    
    imes
}

lazy_static::lazy_static! {
    static ref CURRENT_PACKAGE: Arc<Mutex<String>> = Arc::new(Mutex::new("".to_string()));    
    static ref IME_BLOCKLIST: HashSet<String> = get_system_ime_packages();
}

pub fn get_current_pid() -> i32 {
    CURRENT_PID.load(Ordering::Relaxed)
}

// 在检测到新包名时更新它
fn set_current_package(pkg: &str, pid: i32) {
    *CURRENT_PACKAGE.lock().unwrap() = pkg.to_string();
    CURRENT_PID.store(pid, Ordering::Relaxed);
}

// ==================== [核心：纯 Cgroup 检测逻辑] ====================

/// 判断是否为有效的用户应用包名
fn is_valid_user_app(pkg: &str, ignored_apps: &[String]) -> bool {
    if pkg.is_empty() || !pkg.contains('.') || pkg.starts_with('/') || pkg.starts_with('.') || pkg.contains(':') {
        return false;
    }
    if IME_BLOCKLIST.contains(pkg) {
        return false; 
    }
    if ignored_apps.iter().any(|ignored| pkg == ignored || pkg.contains(ignored)) {
        return false;
    }
    match pkg {
        "com.android.systemui" => false,
        "system_server" => false,
        "surfaceflinger" => false,
        "android.hardware.graphics.composer" => false,
        "com.android.phone" => false,
        "com.android.permissioncontroller" => false,
        // Aegis daemon self-exclusion (legacy name kept for safety).
        "aegis" | "Aegis" | "yumi" => false,
        "com.xiaomi.vtcamera" => false,
        "com.android.providers.media.module" => false,
        "com.google.android.gms.ui" => false,
        "com.xiaomi.mibrain.speech" => false,
        _ => {
            if pkg.contains("magisk") || pkg.contains("mtiodaemon") { return false; }
            if pkg.contains("ads_monitor") { return false; }
            if pkg.contains("inputmethod") { return false; }
            true
        }
    }
}

// 提取核心检测逻辑
fn check_cgroup_path(path: &str, ignored_apps: &[String]) -> Option<(String, i32)> {
    if let Ok(content) = utils::read_file_content(path) {
        let pids: Vec<&str> = content.split_whitespace().collect();
        for pid_str in pids.iter().rev() {
            let cmdline_path = format!("/proc/{}/cmdline", pid_str);
            if let Ok(cmdline) = utils::read_file_content(&cmdline_path) {
                let pkg_name = cmdline.split('\0').next().unwrap_or("").trim();
                if is_valid_user_app(pkg_name, ignored_apps) {
                    let pid = pid_str.parse::<i32>().unwrap_or(0);
                    return Some((pkg_name.to_string(), pid));
                }
            }
        }
    }
    None
}

/// 从 Cgroup 读取前台应用
fn get_focused_app_from_cgroup(ignored_apps: &[String]) -> Result<(String, i32), Box<dyn Error>> {
    let paths = [
        "/dev/cpuset/top-app/cgroup.procs",
        "/sys/fs/cgroup/cpuset/top-app/cgroup.procs",
        "/dev/stune/top-app/cgroup.procs"
    ];

    let cached = VALID_CGROUP_IDX.load(Ordering::Relaxed);
    if cached < paths.len() {
        if let Some(res) = check_cgroup_path(paths[cached], ignored_apps) {
            return Ok(res); 
        }
    }

    for (i, path) in paths.iter().enumerate() {
        if i == cached { continue; }
        if let Some(res) = check_cgroup_path(path, ignored_apps) {
            VALID_CGROUP_IDX.store(i, Ordering::Relaxed);
            return Ok(res);
        }
    }
    
    Err("No valid app found".into())
}

// ==================== [辅助函数] ====================

fn determine_mode(config: &RulesConfig, current_package: &str) -> String {
    // Aegis semantics:
    //  * Per-app rule ALWAYS wins. An app explicitly assigned "fas" gets FAS
    //    (probes wake on detection) regardless of the global FAS switch.
    //  * fas_enabled only gates the GLOBAL fas mode. When global_mode=fas and
    //    the switch is off, non-assigned apps fall back to balance — so the
    //    user can disable blanket FAS while keeping FAS for chosen apps.
    if config.dynamic_enabled {
        if let Some(app_mode) = config.app_modes.get(current_package) {
            return app_mode.clone();
        }
    }
    let global = config.global_mode.clone();
    if global == "fas" && !config.fas_enabled {
        "balance".to_string()
    } else {
        global
    }
}

pub fn get_default_rules() -> RulesConfig {
    RulesConfig {
        aegis_scheduler: true,
        fas_enabled: true,
        dynamic_enabled: true,
        global_mode: "balance".to_string(),
        app_modes: HashMap::new(),
        ignored_apps: Vec::new(),
        fas_rules: super::config::FasRulesConfig::default(),
    }
}

pub fn watch_config_file(
    config_arc: Arc<Mutex<RulesConfig>>,
    force_refresh_arc: Arc<AtomicBool>,
    tx: Sender<DaemonEvent>
) -> Result<(), Box<dyn Error>> {
    let mut inotify = Inotify::init()?;
    let rules_path = config::get_rules_path();
    if !rules_path.exists() { let _ = utils::try_write_file(&rules_path, ""); }

    // Aegis robustness fix: editors that replace the file atomically
    // (`sed -i`, `cp`, WebUI atomic save) swap the inode, which silently kills
    // a watch on the file itself (classic inotify rename trap). Watch the
    // parent directory too and re-bind the file watch whenever it reappears
    // (tail -F semantics), so reloads never die after the first rename save.
    let rules_dir = rules_path
        .parent()
        .ok_or("rules.yaml has no parent directory")?;
    let dir_wd = inotify
        .watches()
        .add(rules_dir, WatchMask::MOVED_TO | WatchMask::CREATE)?;
    let mut file_wd = inotify
        .watches()
        .add(&rules_path, WatchMask::MODIFY | WatchMask::CLOSE_WRITE)
        .ok();
    info!("{}", t_with_args("app-detect-config-watch", &fluent_args!("path" => format!("{:?}", rules_path))));
    let mut buffer = [0u8; 4096];
    loop {
        let mut events = inotify.read_events_blocking(&mut buffer)?.peekable();
        if events.peek().is_none() {
            continue;
        }

        let mut rules_touched = false;
        let mut file_watch_gone = false;
        for event in events {
            if event.wd == dir_wd
                && event.name.as_deref() == Some(std::ffi::OsStr::new("rules.yaml"))
            {
                // New inode appeared (or was recreated): MOVED_TO/CREATE.
                rules_touched = true;
            } else if Some(event.wd) == file_wd {
                if event.mask.contains(EventMask::IGNORED) {
                    file_watch_gone = true;
                } else {
                    rules_touched = true;
                }
            }
        }

        if file_watch_gone {
            // Old file watch died with the replaced inode; drop the stale wd,
            // a fresh one is (re)bound below when the new file is touched.
            file_wd = None;
        }

        if !rules_touched {
            continue;
        }

        // Re-bind to the current inode (no-op when the watch is still valid;
        // required after rename-style saves).
        if file_wd.is_none() && rules_path.exists() {
            file_wd = inotify
                .watches()
                .add(&rules_path, WatchMask::MODIFY | WatchMask::CLOSE_WRITE)
                .ok();
        }

        info!("{}", t("app-detect-change-detected"));
        thread::sleep(Duration::from_millis(100));
        // Drain the burst of events atomic writers emit (MOVED_TO + IGNORED,
        // or multiple MODIFY/CLOSE_WRITE), rebinding the file watch if needed.
        // The inotify fd is non-blocking: no pending events => WouldBlock.
        loop {
            match inotify.read_events(&mut buffer) {
                Ok(events) => {
                    let mut gone = false;
                    for event in events {
                        if event.wd == dir_wd
                            && event.name.as_deref() == Some(std::ffi::OsStr::new("rules.yaml"))
                        {
                            rules_touched = true;
                        } else if Some(event.wd) == file_wd
                            && event.mask.contains(EventMask::IGNORED)
                        {
                            gone = true;
                        }
                    }
                    if gone {
                        file_wd = None;
                    }
                    if file_wd.is_none() && rules_path.exists() {
                        file_wd = inotify
                            .watches()
                            .add(&rules_path, WatchMask::MODIFY | WatchMask::CLOSE_WRITE)
                            .ok();
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e.into()),
            }
        }

        info!("{}", t("app-detect-reloading"));

        let new_config = crate::utils::read_config::<RulesConfig, _>(&rules_path)
                            .unwrap_or_else(|e| {
                                warn!("{}", t_with_args("app-detect-load-failed", &fluent_args!("error" => e.to_string())));
                                get_default_rules()
                            });

        *config_arc.lock().unwrap() = new_config.clone();

        if let Err(e) = tx.send(DaemonEvent::ConfigReload(new_config)) {
            warn!("[Config] Failed to send ConfigReload event: {}", e);
        }

        info!("{}", t("app-detect-reload-success"));
        force_refresh_arc.store(true, Ordering::SeqCst);
    }
}

pub fn app_detection_loop(
    config_arc: Arc<Mutex<RulesConfig>>, 
    screen_state_arc: Arc<Mutex<bool>>,
    force_refresh_arc: Arc<AtomicBool>,
    tx: Sender<DaemonEvent>
) -> Result<(), Box<dyn Error>> {
    info!("{}", t("app-detect-loop-started"));
    
    let temp_sensor_path = utils::find_cpu_temp_path().unwrap_or_default();
    let mut last_package = String::new();
    let mut last_mode = String::new();
    let mut last_screen_state = true; 
    
    // 状态机变量：用于无阻塞防抖
    let mut pending_package = String::new();
    let mut pending_pid = 0;
    let mut debounce_start = Instant::now();
    
    loop {
        let force_refresh = force_refresh_arc.swap(false, Ordering::SeqCst);
        let current_screen_state = { *screen_state_arc.lock().unwrap() };
        
        if current_screen_state != last_screen_state {
            info!("{}", t_with_args("app-detect-screen-changed", &fluent_args!("old" => last_screen_state.to_string(), "new" => current_screen_state.to_string())));
            last_screen_state = current_screen_state;
            let _ = tx.send(DaemonEvent::ScreenStateChange(current_screen_state));

            if current_screen_state {
                last_package.clear();
                pending_package.clear();
                last_mode.clear();
                force_refresh_arc.store(true, Ordering::SeqCst);
            }
        }

        if !current_screen_state {
            // Aegis: screen off -> FAS probes must sleep (Doze uses CLG only).
            FAS_ACTIVE.store(false, Ordering::Relaxed);
            thread::sleep(Duration::from_secs(1));
            continue;
        }
                
        // 合并锁获取：一次拿完所有需要的数据
        let config_snapshot = config_arc.lock().unwrap().clone();
        let ignored_apps = config_snapshot.ignored_apps.clone();

        let (detected_pkg, detected_pid) = get_focused_app_from_cgroup(&ignored_apps)
            .unwrap_or_else(|_| (last_package.clone(), get_current_pid()));

        let mut final_pkg = last_package.clone();
        let mut final_pid = get_current_pid();

        // 无阻塞防抖逻辑
        if detected_pkg != last_package && !detected_pkg.is_empty() {
            if detected_pkg != pending_package || detected_pid != pending_pid {
                pending_package = detected_pkg.clone();
                pending_pid = detected_pid;
                debounce_start = Instant::now();
            } else if debounce_start.elapsed() >= Duration::from_millis(500) {
                final_pkg = pending_package.clone();
                final_pid = pending_pid;
                pending_package.clear();
            }
        } else {
            pending_package.clear();
            if detected_pkg == last_package && detected_pid > 0 {
                final_pid = detected_pid;
            }
        }

        let current_temp = if !temp_sensor_path.is_empty() {
            utils::read_f64_from_file(&temp_sensor_path).unwrap_or(0.0) / 1000.0
        } else { 0.0 };
        
        let pid_changed = final_pid != get_current_pid();
        if last_package != final_pkg || pid_changed || force_refresh {
            if !final_pkg.is_empty() {
                set_current_package(&final_pkg, final_pid);
                let new_mode = determine_mode(&config_snapshot, &final_pkg);

                // Aegis: drive the FAS gate. Frame probes load ONLY for apps
                // that actually resolve to FAS (explicit per-app FAS still works
                // when the global FAS switch is off); otherwise the FPS worker
                // sleeps with zero kernel probes attached (idle power fix).
                FAS_ACTIVE.store(new_mode == "fas", Ordering::Relaxed);

                // force_refresh（配置重载/亮屏恢复）只驱动外层重新计算模式；
                // 模式未变时不重发 ModeChange，避免 "balance -> balance" 冗余事件。
                // CLG 按模式调频；FAS 同模式切换游戏也必须更新 PID 和应用配置。
                if last_mode != new_mode || (new_mode == "fas" && (last_package != final_pkg || pid_changed)) {
                    info!("{}", t_with_args("app-detect-mode-change-pkg", &fluent_args!("old" => last_mode.clone(), "new" => new_mode.as_str(), "pkg" => final_pkg.as_str())));
                    // ModeChange 事件现在携带 pid 字段
                    let _ = tx.send(DaemonEvent::ModeChange {
                        package_name: final_pkg.clone(),
                        pid: final_pid,
                        mode: new_mode.clone(),
                        temperature: current_temp,
                    });
                    last_mode = new_mode;
                }
                last_package = final_pkg;
            }
        }

        thread::sleep(Duration::from_millis(1500));
    }
}
