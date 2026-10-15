# Aegis-Scheduling · 私人定制 Android CPU 调度

> 基于开源项目 **yumi v2.0.2**（<https://github.com/imacte/yumi>，GPL-3.0-or-later）二次开发。
> 守护进程改名 **Aegis**，与「Aegis 电源矩阵」模块族（充电阈值 / 温控 / 后台冻结）集成。
> 本仓库为个人使用的私密备份，目标：**静态看电视/看小说/低速滑信息流时低功耗，触摸滑动时不掉帧**，换设备或后续调参时一键找回全部上下文。

- 验证主力机：**小米 MIX Fold 3（babylon）· 骁龙 8 Gen2 领先版 SM8550-AC · Android 16 · KernelSU**
- 版本标识：`v2.0.2-aegis`（versionCode 20003），当前调度核心迭代到 **CLG v7（2026-10-15 真机验证）**
- 可直接刷入的整包：[`prebuilt/Aegis-scheduling-2.0.2-aegis-20261015.zip`](prebuilt/Aegis-scheduling-2.0.2-aegis-20261015.zip)

---

## 目录

1. [核心思路](#1-核心思路)
2. [仓库结构](#2-仓库结构)
3. [相对原版 yumi 2.0.2 的全部更新](#3-相对原版-yumi-202-的全部更新)
4. [8Gen2 定制参数一览](#4-8gen2-定制参数一览)
5. [真机实测数据（2026-10-15 CLG v7）](#5-真机实测数据2026-10-15-clg-v7)
6. [编译方法](#6-编译方法)
7. [刷入与热部署](#7-刷入与热部署)
8. [调参手册](#8-调参手册)
9. [换设备移植流程](#9-换设备移植流程)
10. [设计红线与注意事项](#10-设计红线与注意事项)

---

## 1. 核心思路

**v7：一条连续增益曲线 + 一个加速条件，无状态、无驻留、无模式切换。**

```
target_perf = 簇真实 max_util × gain        （再夹在 perf_floor / perf_ceil 之间）
```

增益 `gain` 是随负载单调上升的一条直线，全程连续无阶跃：

| 真实负载 raw_util | gain | 含义 |
|---|---|---|
| ≤ `down_threshold` | `low_load_gain`（0.40~0.75） | 静态/轻载深度压频 |
| ≥ `up_threshold` | `headroom_factor`（1.15~1.30） | 重载留帧余量 |
| 两阈值之间 | 线性插值 | 平滑过渡 |

**升降频对称，没有驻留状态：**

- **升频**默认要连续 `up_rate_limit_ticks` 拍确认，滤掉后台单拍满载毛刺；唯一例外（加速通道）：前台 App 最忙线程 ≥ `interact_fg_util` **且** 单拍负载跳升 ≥ `spike_jump_threshold`（真实手势起步）→ 当拍一步到目标。注意到的是公式算出的**被压低的目标**，不是拉满——MT 慢滑起步一步到 614MHz，而不是 2GHz。
- **降频**：raw < `down_fast_threshold` 立即快速沉底（×4~4.5）；滞回带内按 `slow_down_scale` 步长下降。负载一掉频率就回去，不需要任何 hold。

**为什么 v6 的交互态被删掉（v7 的由来）**：v6 维护 onset/sustaining/hold 状态机，前台线程偶尔一拍冲过闸门就触发"首拍 rate=1.0 × headroom 1.3"，真实 0.4 的负载被算成 ~1.7GHz 的需求，再被 0.8s 驻留粘住。实测红果静态播放中核 72% 时间在 1500+（中位 1651），MT 慢滑这种 util 仅 0.28 的轻列表被踹到 2093/2726。补丁越叠越多（首拍拉满、驻留续期、交互态禁快降……），正是"一个谎要用一万个谎圆"。v7 全部删掉，频率只跟随真实负载。

前台闸门仍保留为增益之外的唯一信号：桌面无操作时 MIUI 后台 churn 把整簇 util 冲到 0.7+，但最忙前台 App 线程只有 0.03~0.05；滑动时 0.4~1.0；视频硬解约 0.10~0.15（低于闸门，天然走低功耗路径）。

---

## 2. 仓库结构

```
├─ src/                     Rust 守护源码（Aegis 本体）
│  ├─ scheduler/
│  │  ├─ cpu_load_governor.rs   ★ CLG 日常调频核心（v7 无状态重写，见 §3.2）
│  │  ├─ config.rs              ★ CLG 配置（v7 精简字段，见 §3.2）
│  │  ├─ mod.rs                 ★ 调度线程（tick 100ms / 模式切换刷 module.prop）
│  │  ├─ scheduler.rs / monitor_health.rs
│  │  └─ fas/                   FAS 游戏帧感知调度（PID/帧管线/档位，未改算法）
│  ├─ monitor/
│  │  ├─ app_detect.rs      ★ 前台识别 + FAS 闸门 + inotify 热重载修复
│  │  ├─ cpu_monitor.rs     ★ 采样 tick 200ms→100ms
│  │  └─ fps_monitor.rs     ★ eBPF 帧探针懒加载/零空转
│  └─ common.rs             模块根目录注释（路径回溯按 Aegis 命名）
├─ yumi-ebpf/               eBPF 探针程序（未改）
├─ module/                  刷包骨架
│  ├─ config/config.yaml    ★ 8Gen2 四模式 CLG 定值（§4.1）
│  ├─ rules.yaml            ★ 8Gen2 FAS 定值 + FAS 开关语义（§4.2）
│  ├─ service.sh / customize.sh / uninstall.sh / module.prop  ★ Aegis 品牌化
│  └─ config/i18n/          守护日志中文化
├─ webui/                   Vue WebUI 源码（品牌化 + FAS 开关 + 充电/温控页）
│  └─ src/views/ChargeView.vue  ★ 修复充电表单轮询回填覆盖编辑（见 §3.6）
├─ xtask/                   打包任务（产物名 yumi→Aegis、aegis-*.zip）
├─ prebuilt/                ★ 真机验证过的可刷整包（含充电阈值/温控全家桶）
├─ tools/
│  └─ aegis_cpu_probe.sh    ★ 换新设备时的硬件探测脚本
└─ docs/
   ├─ Aegis调参说明.txt      ★ 完整参数调参手册（v7 版，每个参数的影响）
   ├─ monitor-recovery.md   上游文档
   └─ upstream-README*.md   原版 yumi 的 README 备份
```

带 ★ 的是相对原版改动或新增的内容。

---

## 3. 相对原版 yumi 2.0.2 的全部更新

### 3.1 品牌与模块集成（无行为风险的改名）

- 守护二进制 `yumi` → **`Aegis`**：`Cargo.toml` 的 `[[bin]] name`、`xtask` 打包/拷贝路径、`module/.gitignore` 全部同步；包名 `yumi`→`aegis`，描述改为 "Android CPU scheduling daemon (yumi core rebrand)"。
- `module.prop`：id `aegis`、名称「Aegis 调度」、版本 `v2.0.2-aegis`(20003)、描述「eBPF 负载调频 · 帧感知调度（Aegis 守护 / FAS 零空转）· 充电阈值 · 温控」，**删除原 updateJson 在线更新地址**（防止被上游更新覆盖定制）。
- `service.sh`：启动 `core/bin/Aegis`；清理旧进程时同时 `killall -9 Aegis` 和 `yumi`（防双守护并存）；删掉原版已注释的 Oiface/Joyose/debug 启动等死代码。
- `uninstall.sh`：去掉对 Oiface/Joyose 的恢复操作（本机从来没禁用过），只留卸载提示。
- `customize.sh`、`config/i18n/zh.ftl|en.ftl`、守护日志文案：全部 Aegis 中文化；`config.yaml` 默认语言改 zh、作者/配置名改 aegis_config。
- `rules.yaml` 字段 `yumi_scheduler` → `aegis_scheduler`，serde 加了 `alias = "yumi_scheduler"`，**旧规则文件直接兼容**。
- WebUI：整体品牌化为 Aegis，首页/应用规则页/日志页中文化重排，新增充电（ChargeView）/温控（ThermalView）页面入口与 shield 图标，FAS 开关与模式切换接入。

### 3.2 CLG 日常调频核心（`src/scheduler/cpu_load_governor.rs`）

**v1→v6 的演进**：低载增益压缩（v5）→ 前台闸门 + 交互态状态机 + 驻留（v6）。v6 解决了滑动起步迟滞，但引入了静态空高（见 §1）。

**v7 重写（当前版本，无状态）**：

- 删除 `ClusterState.hold` 驻留计数与 onset/sustaining/interactive 全部判定分支；删除"交互态首拍 rate=1.0""交互态禁快降""交互态给满 headroom"三套分叉。
- 删除配置字段 `interact_hold_ticks`、`spike_decay`、`headroom_ramp`（前两者随状态机一起消失，后者在 v6 就已是死字段）。
- 每簇每拍只做：算 raw_util 与 jump → 增益线性插值得 target_perf → 升频（jump 免确认通道 / 普通确认+平滑）或降频（快速沉底 / 正常 / 滞回带慢速）→ 就近取档写频。
- 保留字段：`up/down_threshold`、`low_load_gain`、`headroom_factor`（增益曲线两端）、`spike_jump_threshold` + `interact_fg_util`（jump 通道双条件）、`smoothing_up/down`、`up/down_rate_limit_ticks`、`slow_up/down_scale`、`down_fast_threshold/mult`、`perf_floor/ceil/init`、`up_jump_threshold`。
- 尖峰衰减（`spike_decay`）随交互态一并删除：后台单拍毛刺改由"前台闸门 + up_wait 连续拍确认"双重过滤，前台不忙的单拍 jump 顶不起频率；真实手势的 jump 本来就不该衰减。
- `on_load_update(&core_utils, fg_max_util)` 签名不变（fg 利用率仍为 jump 通道使用）。
- tick 调试日志计数 50（约 5s 一条）。

### 3.3 FAS「零空转」（`app_detect.rs` + `fps_monitor.rs`）

原版守护一启动就常驻加载 eBPF 帧探针（程序 + 每 CPU perf buffer + uprobe），不打游戏也有持续空载开销（实测约 0.2W）。改动：

- 新增原子闸门 `FAS_ACTIVE` + `is_fas_active()`：仅在**亮屏**且**当前前台 App 解析出的模式确实是 fas** 时为 true；息屏立即置 false。
- `fps_monitor` 重写为「睡眠槽位」状态机：闸门 false 时**完全卸载探针、关闭 perf buffer、不轮询**，1s 一醒；进入 FAS App 才懒加载；加载失败不杀死 worker，发 `FpsProbeUnavailable` 后 5s 重试；退出 FAS 时 detach 并通知调度器释放残余锁频。
- 新增规则字段 `fas_enabled`，模式判定 `determine_mode` 语义明确为：
  - **App 单独规则永远优先**：被 `app_modes` 显式指定 fas 的游戏，不管全局开关都唤醒 FAS；
  - `fas_enabled` 只管全局：`global_mode: "fas"` 但开关 false 时，普通 App 回落 balance（本机定值 `fas_enabled: false`）。
- `watch_config_file` 修复 **inotify rename 陷阱**：`sed -i`/`cp`/WebUI 原子保存会换 inode，原版只 watch 文件本身，热重载在第一次原子保存后就永久失效。改为同时 watch 父目录（MOVED_TO/CREATE），文件 inode 更换后自动重新绑定（tail -F 语义），并正确排空原子写产生的事件突发；缓冲区 1024→4096。
- 模式切换落盘 `current_mode.txt` 后立即 spawn `update_prop` 刷新 module.prop，KSU 管理器里显示的当前模式实时更新（原版只在开机刷一次）。
- 前台识别黑名单加入 `aegis`/`Aegis`（保留 `yumi` 兼容），避免把守护自身识别成前台 App。

### 3.4 采样与调度周期 200ms → 100ms

- `monitor/cpu_monitor.rs`：CPU 采样 tick 200ms→**100ms**，快照计数 50。
- `scheduler/mod.rs`：调度线程 `recv_timeout` 200ms→100ms。
- FAS 安全性已核实：`fas/controller.rs` 全部基于真实 elapsed/Instant 计时（util 采样 1000ms、cold_boot_ms、verify 1500ms、帧事件驱动的 freq_force_counter），与 tick 无关，tick 减半不影响 FAS。

### 3.5 8Gen2 / SM8550 整机定值

见下一节，全部落在 `module/config/config.yaml` 与 `module/rules.yaml`（与 prebuilt 整包一致）。

### 3.6 WebUI 充电页编辑被弹回修复（`webui/src/views/ChargeView.vue`）

现象：充电阈值页里「安全余量 / 保底电量 / 生效窗口」（以及其他字段）清空数字重新输入时，会被瞬间恢复成旧值。根因：页面每 3 秒轮询磁盘上的 `charge_guard.conf` 并整体回填表单，而旧代码的 `dirty` 标志只在"点击保存"期间置位，**用户编辑时从不置位**——编辑超过 3 秒未保存就被磁盘旧值覆盖（预约区三个字段因为要先开开关、编辑时间最长，最容易撞上）。修复：

- 对 `form` 增加 `flush:'sync'` 深度 watch，用户任意修改立即置 `dirty=true`，轮询回填在 dirty 期间跳过；`fillForm()` 程序化赋值用 `populating` 护栏避免误标脏；保存成功后清 dirty。
- 顺带修正 `sched_margin=0`、`sched_floor=0`（合法值，0 保底=不设保底）被 `parseInt(...) || 默认值` 误吞成 15/20 的问题，改为 `Number.isNaN` 判定。
- 因构建机无 Node 环境，prebuilt 内 `webroot/index.html` 为等价的手工修补 bundle（13 个表单更新处理器统一置脏 + NaN 判定），源码以 `ChargeView.vue` 为准。

---

## 4. 8Gen2 定制参数一览

### 4.1 CLG 四模式（`module/config/config.yaml`，v7 定值）

| 参数 | powersave | balance | performance | fast |
|---|---|---|---|---|
| up_threshold | 0.75 | 0.70 | 0.55 | 0.01（锁满） |
| down_threshold | 0.45 | 0.40 | 0.35 | 0.01 |
| smoothing_up / down | 0.50 / 0.45 | 0.65 / 0.35 | 0.85 / 0.20 | 1.0 / 0.01 |
| up / down rate(ticks) | 2 / 2 | 2 / 2 | 1 / 4 | 1 / 10 |
| headroom_factor（增益高端） | 1.15 | 1.15 | 1.30 | 2.0 |
| perf_floor / ceil / init | 0 / 0.70 / 0.25 | 0 / 1.0 / 0.35 | 0.10 / 1.0 / 0.55 | 1.0/1.0/1.0 |
| **low_load_gain（增益低端）** | **0.40** | **0.55** | **0.75** | 1.0 |
| **spike_jump_threshold** | **0.40** | **0.35** | 0.35 | 0.35 |
| interact_fg_util（jump 闸门） | 0.25 | 0.25 | 0.25 | 0.25 |
| up_jump / slow_up / slow_down | 0.35/0.10/**0.50** | 0.35/0.15/**0.50** | 0.35/0.15/0.35 | — |
| down_fast_thresh / mult | 0.30 / 4.5 | 0.30 / 4.0 | 0.15 / 3.0 | — |

三档性格：省电档压得最深（gain 0.40 / ceil 0.70 / jump 门槛最高 0.40），只有真实手势起步才直通，回退快；标准档压频略浅、不封上限、jump 0.35 更跟手；性能档低门槛、快升、缓降（down_wait=4）并保留 0.10 底频，档位性格即"倾向驻留高位"。

v6 表里的 `interact_hold_ticks`、`spike_decay`、`headroom_ramp` 已从配置与代码中删除。

### 4.2 FAS 游戏档（`module/rules.yaml`，要点）

- 三簇算力权重（`auto_capacity_weight: false` 显式固化，换 ROM 不漂）：**1.00 / 2.43 / 2.69**，由 capacity 266/811/1024 按 `1+√(cap/266−1)` 换算；
- perf 范围按三簇频率表换算：`perf_floor 0.30`（小≈820/中≈1190/超大≈1425，游戏进场不卡）、`perf_init 0.50`（≈1162/1651/1978）、ceil 1.0、cold_boot 0.85；
- PID kp/ki/kd = 0.05/0.01/0.006（上游默认，未动）；
- 游戏帧率档/加载保护/快速衰减等帧计数参数保持上游默认；
- `global_mode: "balance"`、`fas_enabled: false`（默认零探针，仅指定的 4 款游戏唤醒 FAS）。

### 4.3 硬件频率表（perf 归一化依据）

| policy | 核心 | 频点范围 | 档位 | capacity |
|---|---|---|---|---|
| policy0 | cpu0-2 · 3×A510 小核 | 307200~2016000 kHz | 16 | 266 |
| policy3 | cpu3-6 · 2×A715+2×A710 中核 | 499200~2803200 kHz | 19 | 811 |
| policy7 | cpu7 · 1×X3 超大核 | 595200~3360000 kHz | 20 | 1024 |

8Gen2 标准版（3.2GHz）与领先版（3.36GHz）capacity 表与调频机制相同，配置 AB/AC 通刷，perf 归一化后自动落各自真实最高频。

---

## 5. 真机实测数据（2026-10-15 CLG v7）

### 5.1 钉频对照——酷安信息流的真实频率门槛

方法论：停掉守护，把大核 policy7 封死在 864MHz（逼渲染线程留在中核，排除 MIUI 触摸加速把任务迁去 X3 的干扰），中核 policy3 逐档钉频跑同一组 16 次慢拖+快滑，`dumpsys gfxinfo com.coolapk.market` 量帧：

| p3 钉频 | 95 / 99 分位 | Janky(legacy) | 结论 |
|---|---|---|---|
| 1171 MHz | 28 / 42 ms | 97.6% | 明显卡 |
| 1401 MHz | 23 / 32 ms | 82.0% | 卡 |
| 1651 MHz | 18 / 26 ms | 36.7% | 勉强 |
| **1920 MHz** | **17 / 23 ms** | **19.6%** | **流畅，省电档按此线给频** |

对照实验同时确认：酷安滑动时大核利用率仅 8~16%，p7 被踹到 2GHz+ 纯属浪费。

### 5.2 v6 → v7 频率对比（均为省电档，酷安/红果由规则自动匹配 powersave）

**MT 管理器（bin.mt.plus，轻量文件列表，rules.yaml 已显式钉到 powersave 档）**：

| 指标 | v6 交互态 | v7 |
|---|---|---|
| 静止 p3 中位 | 499 | 499 |
| 慢滑 p3 中位 | 1171（常跳 2093/2726） | **499~614**，≥1500 仅 3% |
| 大核 ≥2GHz 占比 | 频繁 | 3% |

**红果短剧（com.phoenix.read，静态播放 40s，87 个采样）**：

| 指标 | v6 | v7 |
|---|---|---|
| p3 中位 | 1651 | **614** |
| p3 ≥1500 占比 | 72% | **16%** |
| p3 ≤1171 占比 | 少数 | **78%** |
| 前台最忙线程 util | ~0.14 | ~0.15（信号一致，证明负载没变，是给频变了） |

**酷安（com.coolapk.market，16 次慢拖+快滑）**：

| 指标 | v7 原参 | v7 省电档定参（down_wait 3→2 / slow_down 0.40→0.50 / jump 0.35→0.40） |
|---|---|---|
| p3 中位 | 2054 | **1920**（=钉频流畅线） |
| 顶格 2188 占比 | 24% | **16%** |
| 大核 ≥2GHz 占比 | 7% | **3%** |
| GFX Janky / Missed Vsync | 0 / 0 | **0 / 0** |
| 95 / 99 分位 | 17 / 24 ms | **17 / 22 ms** |

调参方法备注：先录制逐拍簇 util/前台线程 util 轨迹，本地按 governor 公式回放扫参数，再只把候选档上真机 A/B（含 GFX）；曾试中位 1786 的更激进档，零丢帧但 95 分位升到 18ms、legacy 卡顿率明显上涨，低于钉频流畅线，弃用。

### 5.3 稳定性

- 守护连续运行零 panic（`grep -c panic logs/daemon.log` = 0）；
- 静态桌面 p3 贴 499~614，打字/微信消息场景主观可接受（用户确认）。

---

## 6. 编译方法

Windows 主机交叉编译（已验证环境）：

```powershell
$env:RUSTFLAGS="-C default-linker-libraries"
$env:ANDROID_NDK_HOME="C:\android-ndk-r29"
$env:PATH="C:\work\tools\bpflinker;" + $env:PATH
cargo +nightly ndk --platform 26 -t arm64-v8a build -Z build-std -r
```

- 产物：`target/aarch64-linux-android/release/Aegis`
- 工具链：nightly + `cargo-ndk` + Android NDK r29（platform 26 = Android 8.0）
- 唯一已知 warning：`app_detect.rs` 的 `rules_touched` 未读（无害，勿动）
- WebUI：`webui/` 下 `npm install && npm run build`，产物 dist 由 xtask 收进模块 webroot
- 整包打包：用 xtask（产出 `aegis-<ver>-*.zip`）；手工打包时**禁用 PowerShell 的 Compress-Archive**（反斜杠条目在 Android 上乱），用：
  ```powershell
  $names = Get-ChildItem -LiteralPath <包目录> -Force | ForEach-Object Name
  tar.exe -a -cf out.zip -C <包目录> @names
  ```

prebuilt 里的整包 = 调度模块 + 充电阈值/温控脚本与二进制（charge_guard、thermal_manager、busybox、yc/uperf 配置等）合并后的版本，结构以整包为准。

## 7. 刷入与热部署

- **正常刷入**：KernelSU 管理器从 zip 安装，重启。`customize.sh` 会保留已有的模式/FAS 开关/应用规则与充电阈值配置。命令行：
  ```sh
  /data/adb/ksu/bin/ksud module install /sdcard/Aegis-scheduling-*.zip
  ```
- **热部署（不重启替换守护，调参迭代用）**：必须先 kill 再 cp，否则 ETXTBSY：
  ```sh
  adb push Aegis /data/local/tmp/Aegis.new
  adb shell su -c 'killall -9 Aegis; cp /data/local/tmp/Aegis.new /data/adb/modules/aegis_power_matrix/core/bin/Aegis;
                   chmod 755 /data/adb/modules/aegis_power_matrix/core/bin/Aegis;
                   setsid nohup /data/adb/modules/aegis_power_matrix/core/bin/Aegis >/dev/null 2>&1 &'
  ```
- **注意**：`config.yaml` 只在守护启动和切模式时读取，直接 cp 配置不会热重载（inotify 热重载管的是 rules.yaml/模式切换链路）；改完配置需重启守护。
- live 模块路径：`/data/adb/modules/aegis_power_matrix/`；刷入待生效：`/data/adb/modules_update/`
- 日志：`logs/daemon.log`（log4rs 落盘；service.sh 停了 logd 也能看）。临时开 DEBUG：sed 改 `config/config.yaml` 的 `loglevel: "DEBUG"`，守护约 2s 内热重载，**测完务必改回 INFO**。
- 切模式：改 `rules.yaml` 的 `global_mode`（守护 3s 轮询）或 WebUI；直接改 `current_mode.txt` 无效（它是守护写出的状态）。

## 8. 调参手册

完整参数说明见 [docs/Aegis调参说明.txt](docs/Aegis调参说明.txt)（含 perf↔频率换算、每个参数调高/调低的影响、推荐范围与调参纪律）。v7 最常动的三个：

- `low_load_gain`：增益曲线低端，静态压频力度。静态还想更省电 → 0.40（低于 0.4 非触摸前台重活会变慢）；觉得安装/计算变慢 → 0.60。
- `spike_jump_threshold`：手势起步当拍升频的跳变门槛。滑动觉得粘高档空高 → 往 0.40 调；慢滑开头觉得肉 → 往 0.30 调（只影响"到得多快"，不影响到多高）。
- `slow_down_scale`：滞回带内下降速度。信息流滑动中频率回落太慢 → 往 0.50~0.60 调。

原则：**流畅靠「低地板 + 连续增益曲线 + jump 当拍起步 + 快速沉底」，不要抬 perf_floor，也不要重新引入驻留/状态补丁**。怀疑某场景给频不够时，先钉频 + GFX 量出真实流畅线（见 §5.1 酷安实验），再反推增益。

## 9. 换设备移植流程

1. 把 [`tools/aegis_cpu_probe.sh`](tools/aegis_cpu_probe.sh) 推手机跑（脚本头有用法），拿到：分簇/policy 归属、每簇硬件频率表、每核 cpu_capacity。
2. 按 `权重 = 1 + √(本簇cap/最小簇cap − 1)` 算各簇权重，写入 `rules.yaml` 的 `cluster_profiles` 并置 `auto_capacity_weight: false`；更新 `rules.yaml` 头部与 `config.yaml` 头部频率注释。
3. perf↔频率换算：`频率 = 最低频 + perf×(最高频−最低频)`，据此重定 FAS 的 perf_floor/init 与四模式 perf_init；CLG 的比例类参数（阈值/增益/jump 门槛）跨 SoC 通用，一般不用动。
4. 编译（§6），刷入后用 `dumpsys gfxinfo <pkg> reset` + 滑动脚本验证起步延迟与 jank；重负载信息流建议按 §5.1 钉频量一次真实流畅线再定省电档 ceil/gain，最后按 §8 微调到主观手感。

## 10. 设计红线与注意事项

- 温控阈值、充电阈值（charge_guard 配置）**绝不因调度问题改动**；它们由模块族独立组件管理。
- 静态省电是第一诉求（小核 800MHz 级别），修丢帧不得靠抬高静态 floor。
- perf 写频是 min=max 钉频（performance governor）：升频先 max 后 min，降频先 min 后 max；取档 `find_nearest_freq` 就近。
- 低增益压频的已知代价：App 在前台做非触摸重活（JS 计算、安装）且前台线程 util 低于闸门、又没有大跳变时会被压频——无帧延迟约束，可接受。
- 本仓库为私人使用，不处理社群 Issue；上游许可证 GPL-3.0-or-later 继续生效，原作者 yuki。
