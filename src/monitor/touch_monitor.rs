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

//! 触摸事件监听（evdev）。
//!
//! CLG 是 100ms 采样 + "跟随已观测负载"变频，点按瞬间还没采到负载，
//! 会把频率留在低频上，导致点按到首帧多出约 120ms。官方调度的 walt
//! governor 有 input boost（touch-down 当拍抬频），这里用 evdev 原始事件
//! 补上同一个信号，只做一件事：手指按下 → 发 UserTouch 事件。
//!
//! 只读打开（不 EVIOCGRAB，不干扰系统输入分发）；只认 BTN_TOUCH 落下和
//! ABS_MT_TRACKING_ID >= 0，键盘/侧键/传感器的事件一律忽略。

use std::error::Error;
use std::fs::File;
use std::io::Read;
use std::os::unix::io::AsRawFd;
use std::sync::mpsc::Sender;
use std::time::{Duration, Instant};

use log::{info, warn};

use crate::common::DaemonEvent;

const EV_KEY: u16 = 0x01;
const EV_ABS: u16 = 0x03;
const BTN_TOUCH: u16 = 0x14a; // 330
const ABS_MT_TRACKING_ID: u16 = 0x39; // 57
/// struct input_event: timeval(16) + type(2) + code(2) + value(4) = 24 (64-bit)
const EVENT_SIZE: usize = 24;
/// 同一次按下会连发多个 ABS_MT 事件，做最小去抖；不影响连续点按（>300ms 间隔）
const TOUCH_COOLDOWN: Duration = Duration::from_millis(300);

pub fn start_touch_loop(tx: Sender<DaemonEvent>) -> Result<(), Box<dyn Error>> {
    let mut devices: Vec<File> = Vec::new();
    match std::fs::read_dir("/dev/input") {
        Ok(entries) => {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if !name.starts_with("event") {
                    continue;
                }
                let path = entry.path();
                match File::open(&path) {
                    Ok(file) => devices.push(file),
                    Err(e) => warn!("[Touch] open {} failed: {e}", path.display()),
                }
            }
        }
        Err(e) => warn!("[Touch] read /dev/input failed: {e}"),
    }
    info!("[Touch] watching {} input devices", devices.len());
    if devices.is_empty() {
        return Ok(());
    }

    let mut buf = [0u8; 4096];
    let mut last: Option<Instant> = None;

    loop {
        let mut pfds: Vec<libc::pollfd> = devices
            .iter()
            .map(|f| libc::pollfd { fd: f.as_raw_fd(), events: libc::POLLIN, revents: 0 })
            .collect();
        let rc = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as libc::nfds_t, 1000) };
        if rc <= 0 {
            continue;
        }
        for i in 0..devices.len() {
            if pfds[i].revents & libc::POLLIN == 0 {
                continue;
            }
            let n = match devices[i].read(&mut buf) {
                Ok(n) => n,
                Err(_) => continue,
            };
            let mut down = false;
            for chunk in buf[..n].chunks_exact(EVENT_SIZE) {
                let ev_type = u16::from_le_bytes([chunk[16], chunk[17]]);
                let code = u16::from_le_bytes([chunk[18], chunk[19]]);
                let value = i32::from_le_bytes([chunk[20], chunk[21], chunk[22], chunk[23]]);
                if (ev_type == EV_KEY && code == BTN_TOUCH && value == 1)
                    || (ev_type == EV_ABS && code == ABS_MT_TRACKING_ID && value >= 0)
                {
                    down = true;
                }
            }
            if down && last.map_or(true, |t| t.elapsed() >= TOUCH_COOLDOWN) {
                last = Some(Instant::now());
                let _ = tx.send(DaemonEvent::UserTouch { sampled_at: last.unwrap() });
            }
        }
    }
}
