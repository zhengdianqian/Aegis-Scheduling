# Aegis-Scheduling · 私人定制 Android CPU 调度

> 基于开源项目 **yumi v2.0.2**（<https://github.com/imacte/yumi>，GPL-3.0-or-later）二次开发。
> 守护进程改名 **Aegis**，与「Aegis 电源矩阵」模块族（充电阈值 / 温控 / 后台冻结）集成。
> 本仓库为个人使用的私密备份，目标：**静态看电视/看小说/低速滑信息流时低功耗，触摸滑动时不掉帧**，换设备或后续调参时一键找回全部上下文。

- 验证主力机：**小米 MIX Fold 3（babylon）· 骁龙 8 Gen2 领先版 SM8550-AC · Android 16 · KernelSU**
- 版本标识：`v2.0.2-aegis`（versionCode 20003），当前调度核心迭代到 **CLG v6（2026-10-15 真机验证）**
- 可直接刷入的整包：[`prebuilt/Aegis-scheduling-2.0.2-aegis-20261015.zip`](prebuilt/Aegis-scheduling-2.0.2-aegis-20261015.zip)

---

## 目录

1. [核心思路](#1-核心思路)
2. [仓库结构](#2-仓库结构)
3. [相对原版 yumi 2.0.2 的全部更新](#3-相对原版-yumi-202-的全部更新)
4. [8Gen2 定制参数一览](#4-8gen2-定制参数一览)
5. [真机实测数据（2026-10-15 CLG v6）](#5-真机实测数据2026-10-15-clg-v6)
6. [编译方法](#6-编译方法)
7. [刷入与热部署](#7-刷入与热部署)
8. [调参手册](#8-调参手册)
9. [换设备移植流程](#9-换设备移植流程)
10. [设计红线与注意事项](#10-设计红线与注意事项)

---

## 1. 核心思路

调度分两种状态，参数完全分治：

| | 交互态（触摸/滑动/前台渲染） | 非交互态（静态/后台/视频） |
|---|---|---|
| 利用率 | **原始值**，不衰减不压缩 | 尖峰衰减 + 全负载 ×`low_load_gain` 压缩 |
| headroom 余量 | 给满（1.15~1.45） | 恒 1.0 |
| 升频 | 旁路确认，**突增首拍一步到位** | 走 up_wait 确认 + 平滑 |
| 降频 | 只允许匀速平滑回落，**禁止快速沉底** | 低载 ×4~4.5 快速沉底 |
| 进入条件 | 前台最忙 App 线程 ≥`interact_fg_util` 且簇负载 ≥`down_fast_threshold` | 其余全部时间 |
| 驻留 | `interact_hold_ticks` 拍（8 拍=0.8s），前台渲染中持续续期 | — |

前台闸门是省电的关键：实测桌面无操作时 MIUI 后台 churn 会把小核整簇利用率冲到 0.7~0.81，但**最忙前台 App 线程只有 0.03~0.05**；滑动时前台线程 0.24~0.48；视频硬解线程 0.10~0.15（低于闸门，天然走低功耗路径）。

---

## 2. 仓库结构

```
├─ src/                     Rust 守护源码（Aegis 本体）
│  ├─ scheduler/
│  │  ├─ cpu_load_governor.rs   ★ CLG 日常调频核心（交互态重写，见 §3.2）
│  │  ├─ config.rs              ★ CLG 配置（新增 3 个字段）
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
├─ webui/                   Vue WebUI 源码（品牌化 + FAS 开关 + 充电/温控页入口）
├─ xtask/                   打包任务（产物名 yumi→Aegis、aegis-*.zip）
├─ prebuilt/                ★ 真机验证过的可刷整包（含充电阈值/温控全家桶）
├─ tools/
│  └─ aegis_cpu_probe.sh    ★ 换新设备时的硬件探测脚本
└─ docs/
   ├─ Aegis调参说明.txt      ★ 完整参数调参手册（每个参数调高/调低的影响）
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

### 3.2 CLG 日常调频核心（`src/scheduler/cpu_load_governor.rs`，v1→v6 的修复全部在此）

**新增 3 个配置字段**（`src/scheduler/config.rs`，均有 serde default + normalize 区间钳制，旧配置缺字段也能跑）：

| 字段 | 默认 | 含义 |
|---|---|---|
| `low_load_gain` | 1.0（代码默认）/ 0.45~0.75（本机定值） | 非交互态目标性能压缩增益，1.0=跟随原负载=旧行为 |
| `interact_hold_ticks` | 8 | 交互驻留拍数，1 tick=100ms，钳制 ≤50；0=关闭交互态 |
| `interact_fg_util` | 0.25 | 前台闸门：最忙前台 App 线程（UI/RenderThread）利用率阈值，0=不校验 |

**逻辑重写（`on_load_update(&core_utils, fg_max_util)`，签名新增前台利用率入参）：**

1. **交互态判定全部基于压缩前原始 util**：
   - 入场 `onset = hold_ticks>0 && 前台线程util≥interact_fg_util && 簇原始util≥down_fast_threshold`。**不要求负载跳变**——低速慢拖是 0.1→0.35 平缓爬升，v3 曾要求 jump 阈值，结果慢拖永远进不了交互态。
   - 驻留续期 `sustaining` 同样要求前台在忙（v6 修复：v5 续期不校验前台，MIUI 后台噪声曾让交互态永久锁死）；松手 fling 惯性动画由前台 RenderThread 天然续期。
   - hold 在判定后倒计时；前台一停，最迟 `interact_hold_ticks`（0.8s）后必退。
2. **交互态**：util 走原始值（旁路尖峰衰减和压缩）；headroom=`headroom_factor` 给满；升频跳过 `up_wait` 确认且 onset 首拍 `rate=1.0` 一步到位（其后拍正常 smoothing_up）；降频只允许匀速 smoothing_down，禁用 ×4.5 快速沉底。
3. **非交互态**：
   - 尖峰衰减保留（`util - jump*(1-spike_decay)`，仅当 jump≥spike_jump_threshold），专门滤后台孤立毛刺；
   - **所有负载水平统一 ×`low_load_gain`**（v5 修复：原代码只在 util<up_threshold 时压缩，但 100ms 分辨率下后台任务单拍可冲 0.7~0.81，照样反复顶高小核）；
   - **headroom 恒为 1.0**（余量是给渲染线程准备的，后台任务不需要；删掉了原版 up_threshold 附近的 headroom_ramp 线性过渡，`headroom_ramp` 字段保留仅为兼容）；
   - 快速沉底、up/down_wait 确认照旧。
4. tick 调试日志计数 25→50（周期减半后保持约 5s 一条）。

**这解决了什么（最初的故障）**：旧链路 200ms 采样里，触摸首拍 util 先被尖峰衰减（0.30/0.35）再被 low_load_gain 压缩（0.45/0.55），两次串联后高速通道判定永远凑不齐，每段滑动开头 2~4 拍（400ms+）给频不足；松手 fast-down ×4.5 又让断续滑动反复沉底——SCENE 一秒一采看着频率匹配占用率，实际持续轻微丢帧。

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

- `monitor/cpu_monitor.rs`：CPU 采样 tick 200ms→**100ms**，快照计数 25→50。
- `scheduler/mod.rs`：调度线程 `recv_timeout` 200ms→100ms。
- FAS 安全性已核实：`fas/controller.rs` 全部基于真实 elapsed/Instant 计时（util 采样 1000ms、cold_boot_ms、verify 1500ms、帧事件驱动的 freq_force_counter），与 tick 无关，tick 减半不影响 FAS。

### 3.5 8Gen2 / SM8550 整机定值

见下一节，全部落在 `module/config/config.yaml` 与 `module/rules.yaml`（与 prebuilt 整包一致）。

---

## 4. 8Gen2 定制参数一览

### 4.1 CLG 四模式（`module/config/config.yaml`）

| 参数 | powersave | balance | performance | fast |
|---|---|---|---|---|
| up_threshold | 0.75 | 0.70 | 0.55 | 0.01（锁满） |
| down_threshold | 0.45 | 0.40 | 0.35 | 0.01 |
| smoothing_up / down | 0.50 / 0.45 | 0.65 / 0.35 | 0.85 / 0.20 | 1.0 / 0.01 |
| up / down rate(ticks) | 2 / 3 | 1 / 2 | 1 / 4 | 1 / 10 |
| headroom_factor | 1.15 | 1.30 | 1.45 | 2.0 |
| perf_floor / ceil / init | 0 / 0.75 / 0.25 | 0 / 1.0 / 0.35 | 0.10 / 1.0 / 0.55 | 1.0/1.0/1.0 |
| up_jump / slow_up / slow_down | 0.35/0.10/0.40 | 0.35/0.15/0.40 | 0.35/0.15/0.35 | — |
| down_fast_thresh / mult | 0.30 / 4.5 | 0.30 / 4.0 | 0.15 / 3.0 | — |
| spike_jump / spike_decay | 0.35 / 0.30 | 0.30 / 0.35 | 0.35 / 0.30 | — |
| **low_load_gain** | **0.45** | **0.55** | **0.75** | 1.0 |
| **interact_hold_ticks** | **8** | **8** | **10** | 15 |
| **interact_fg_util** | **0.25** | 0.25 | 0.25 | 0.25 |

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

## 5. 真机实测数据（2026-10-15 CLG v6）

设置页（com.android.settings，1080×2520）脚本注入滑动 + GFX 统计：

| 指标 | balance | powersave |
|---|---|---|
| Total frames / Janky | 2454 / **1（0.04%）** | 2868 / **1（0.03%）** |
| Missed Vsync | **0** | **0** |
| 95 / 99 分位帧耗时 | 15 / 16 ms | 14 / 15 ms |

频率轨迹（balance）：

- 桌面静态 pins：p0=902400（后台有 churn 时）/ 最低 556800、**p3=614400、p7=595200 贴硬件底**；
- 首次快滑起步：−126ms 时 672/614/730 kHz → **+211ms 已到 1786/2458/2093 kHz**（旧版最坏 400ms+）；
- 快滑段：p0 中位 1555、p3 中位 1536 / max 2458；
- 慢拖段：p0 中位 1555、p3 max 2803、p7 max 3187（重负载大核兜底）；
- 松手后：1s 内退出交互态，POST 1-5s p0 中位 787、5-9s 回到 557，p3 回 499~614；
- 守护连续运行零 panic（`grep -c panic logs/daemon.log` = 0）。

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
- live 模块路径：`/data/adb/modules/aegis_power_matrix/`；刷入待生效：`/data/adb/modules_update/`
- 日志：`logs/daemon.log`（log4rs 落盘；service.sh 停了 logd 也能看）。临时开 DEBUG：sed 改 `config/config.yaml` 的 `loglevel: "DEBUG"`，守护约 2s 内热重载，**测完务必改回 INFO**。
- 切模式：改 `rules.yaml` 的 `global_mode`（守护 3s 轮询）或 WebUI；直接改 `current_mode.txt` 无效（它是守护写出的状态）。

## 8. 调参手册

完整参数说明见 [docs/Aegis调参说明.txt](docs/Aegis调参说明.txt)（含 perf↔频率换算、每个参数调高/调低的影响、推荐范围与调参纪律）。最常动的三个：

- `interact_hold_ticks`：滑动后高频驻留时长。觉得断续滑动松频太早 → 加到 10；觉得松手后太费电 → 降到 5~6。
- `interact_fg_util`：前台闸门。视频播放器被误给频 → 调到 0.30；慢滑开头觉得肉 → 0.20。
- `low_load_gain`：静态压频力度。静态还想更省电 → 0.40（低于 0.4 非触摸前台重活会变慢）；觉得安装/计算变慢 → 0.60。

原则：**流畅靠「低地板 + 交互态快速回收」，不要抬 perf_floor 换流畅**。

## 9. 换设备移植流程

1. 把 [`tools/aegis_cpu_probe.sh`](tools/aegis_cpu_probe.sh) 推手机跑（脚本头有用法），拿到：分簇/policy 归属、每簇硬件频率表、每核 cpu_capacity。
2. 按 `权重 = 1 + √(本簇cap/最小簇cap − 1)` 算各簇权重，写入 `rules.yaml` 的 `cluster_profiles` 并置 `auto_capacity_weight: false`；更新 `rules.yaml` 头部与 `config.yaml` 头部频率注释。
3. perf↔频率换算：`频率 = 最低频 + perf×(最高频−最低频)`，据此重定 FAS 的 perf_floor/init 与四模式 perf_init；CLG 的比例类参数（阈值/gain/驻留）跨 SoC 通用，一般不用动。
4. 编译（§6），刷入后用 `dumpsys gfxinfo <pkg> reset` + 滑动脚本验证起步延迟与 jank，再按 §8 微调到主观手感。

## 10. 设计红线与注意事项

- 温控阈值、充电阈值（charge_guard 配置）**绝不因调度问题改动**；它们由模块族独立组件管理。
- 静态省电是第一诉求（小核 800MHz 级别），修丢帧不得靠抬高静态 floor。
- perf 写频是 min=max 钉频（performance governor）：升频先 max 后 min，降频先 min 后 max；取档 `find_nearest_freq` 就近。
- 非交互态全负载压缩的已知代价：App 在前台做非触摸重活（JS 计算、安装）且前台线程 util<0.25 时会被压频——无帧延迟约束，可接受。
- 本仓库为私人使用，不处理社群 Issue；上游许可证 GPL-3.0-or-later 继续生效，原作者 yuki。
