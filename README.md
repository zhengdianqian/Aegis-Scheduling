# Aegis-Scheduling · 私人定制 Android CPU 调度

> 基于开源项目 **yumi v2.0.2**（<https://github.com/imacte/yumi>，GPL-3.0-or-later）二次开发。
> 守护进程改名 **Aegis**，模块只做三大功能：**eBPF 负载调频 / 帧感知游戏调度 / 充电阈值与温控**（service.sh 仅启动这三者 + logd 空转抑制，无冻结、无其他系统改动）。
> 本仓库为个人使用的私密备份，目标：**静态看电视/看小说/低速滑信息流时低功耗，触摸滑动时不掉帧**，换设备或后续调参时一键找回全部上下文。

- 验证主力机：**小米 MIX Fold 3（babylon）· 骁龙 8 Gen2 领先版 SM8550-AC · Android 16 · KernelSU**
- **当前版本 `4.6.1`（versionCode 10）**：调度核心 **CLG v7 无状态公式 + 4.6 无感调参 + 4.6.1 输入抬频**；充电守护 **charge_guardd v1.7（充满即停·本次插电锁存）**
- 4.6 的三条主线：
  1. **充电**：充满停充后只要没物理拔电，当次插电**不再补电**（彻底消除睡觉时 100%→99%→100% 整夜浮充，见 §3.8）；
  2. **调度**：亮/息屏自适应节拍 + 前台起步免确认 + 息屏红线收紧 + 三模式重定值，目标是官方那种"几乎感觉不到额外耗电"（见 §3.9、§4.1）；
  3. **版本**：模块 / Cargo / update.json / README / 调参手册统一为 4.6，**Releases 自 4.6 起连续编号**（见 §0）。
- 可直接刷入的整包（与 `module/` 目录内容逐一对齐，**刷入时自动按本机硬件适配**，见 §9）：[`prebuilt/Aegis-scheduling-4.6.zip`](prebuilt/Aegis-scheduling-4.6.zip)
- 配套管理 APP（充电阈值控制页面对齐 4.6）：[`prebuilt/Aegis_manager.apk`](prebuilt/Aegis_manager.apk)

---

## 0. 版本对照（避免版本混用）

| 版本 | versionCode | 关键内容 | 状态 |
|---|---|---|---|
| **4.6.1** | **10** | 输入抬频（触摸按下 150ms 0.85 档，配置项 `input_boost_ms`，改后需息屏再亮屏生效）、`down_fast_threshold` 0.25→0.08 消除频率锯齿 | **当前发布** |
| 4.6 | 9 | 充电锁存 v1.7、自适应采样（亮 100ms / 息 500ms）、前台起步免确认、息屏红线 0.35、三模式重定值、文档整篇重写 | 上一版 |
| 4.3 | 6 | 充满即停忽略恢复电量（99% 补电）、预约保底 +10% 滞回、前台识别重构、前台空闲压频与按簇封顶 | 归档 |
| 4.2 | — | 修复"跳过本次预约"变永久跳过 | 归档 |
| 4 / 2 | — | 品牌化、FAS 零空转、fake_temp 事故修复、刷入即适配 | 归档 |
| 上游 yumi 2.0.2 | — | 原始 CLG / FAS / eBPF 框架 | 上游基线 |

**发布规则**：本仓库 Releases **从 4.6 开始连续编号**（4.6 → 4.7 → …）。4.3 及更早的 tag / zip 只作为历史归档，不再补发、不再追加说明；所有新文档、配置注释、日志版本号一律以当前 `module.prop` 的 `version` 为唯一真值。

---

## 目录

1. [核心思路](#1-核心思路)
2. [仓库结构](#2-仓库结构)
3. [相对原版 yumi 2.0.2 的全部更新](#3-相对原版-yumi-202-的全部更新)
4. [8Gen2 定制参数一览](#4-8gen2-定制参数一览)
5. [真机实测与验证](#5-真机实测与验证)
6. [编译方法](#6-编译方法)
7. [刷入与热部署](#7-刷入与热部署)
8. [调参手册](#8-调参手册)
9. [换设备移植流程](#9-换设备移植流程)
10. [设计红线与注意事项](#10-设计红线与注意事项)

---

## 1. 核心思路

**一条连续增益曲线 + 两条免确认通道，无状态、无驻留、无模式切换。**

```
target_perf = 簇真实 max_util × gain        （再夹在 perf_floor / perf_ceil / cluster_ceil 之间）
```

增益 `gain` 是随负载单调上升的一条直线，全程连续无阶跃：

| 真实负载 raw_util | gain | 含义 |
|---|---|---|
| ≤ `down_threshold` | `low_load_gain`（0.42~0.62） | 静态/轻载深度压频 |
| ≥ `up_threshold` | `headroom_factor`（1.05~1.10） | 重载留帧余量 |
| 两阈值之间 | 线性插值 | 平滑过渡 |

**升降频对称，没有驻留状态：**

- **普通升频**：连续 `up_rate_limit_ticks` 拍确认，滤掉后台单拍满载毛刺。
- **免确认通道（仅前台在忙时成立，两种）：**
  1. `touch_jump`：单拍负载跳升 ≥ `spike_jump_threshold` —— 真实手势起步；
  2. `idle_onset`（4.6 新增）：频率还贴在地板上、前台线程刚超闸门 —— 轻应用"第一次点按/滑动"不再等 2 拍确认。
  两条通道到达的都是增益曲线算出的**被压低的目标**，不是拉满（MT 慢滑起步一步到 614MHz，而不是 2GHz）；后台不忙、息屏（top-app 为空）时两条通道都不成立。
- **降频**：raw < `down_fast_threshold` 立即快速沉底（×3~4.5）；滞回带内按 `slow_down_scale` 步长下降。负载一掉频率就回去，不需要任何 hold。

**逐拍无状态的两道上限（4.3 引入，4.6 调整定值）：**

- `background_ceiling`：前台进程树整体空闲（最忙线程 < `interact_fg_util`）的拍额外封顶；
- `cluster_ceil`：按簇独立封顶，超大核可以单独压（省电档 0.60）。

**息屏红线（4.6 收紧）**：息屏后自动把 `perf_ceil` 与每簇 `cluster_ceil` 压到 **0.35**、`background_ceiling` 压到 0.30、降频立即沉底，同时采样与调度节拍降到 500ms。夜间待机不再因为后台同步把大核顶到高频，也让守护自身的唤醒次数降到亮屏时的 1/5。

**前台信号**：读内核维护的 `/dev/cpuset/top-app` 成员集合——前台 App 的**整个进程树**（主进程 + WebView/小程序沙箱渲染子进程）天然全在内，逐线程取 eBPF runtime 最忙者作 `fg_max_util`；零包名硬编码，任何 App 通用，节点缺失时回退旧的主进程扫描。桌面无操作时 MIUI 后台 churn 把整簇 util 冲到 0.7+，但最忙前台线程只有 0.03~0.05；滑动时 0.4~1.0；视频硬解约 0.10~0.15（低于闸门，天然走低功耗路径）。

**为什么 v6 的交互态被删掉（v7 的由来）**：v6 维护 onset/sustaining/hold 状态机，前台线程偶尔一拍冲过闸门就触发"首拍 rate=1.0 × headroom 1.3"，真实 0.4 的负载被算成 ~1.7GHz 的需求，再被 0.8s 驻留粘住。实测红果静态播放中核 72% 时间在 1500+（中位 1651），MT 慢滑这种 util 仅 0.28 的轻列表被踹到 2093/2726。补丁越叠越多（首拍拉满、驻留续期、交互态禁快降……），正是"一个谎要用一万个谎圆"。v7 全部删掉，频率只跟随真实负载；4.6 的改动同样只动"哪一拍给多少、多久醒一次"，**没有再引入任何驻留状态**。

---

## 2. 仓库结构

```
├─ src/                     Rust 守护源码（Aegis 本体）
│  ├─ scheduler/
│  │  ├─ cpu_load_governor.rs   ★ CLG 日常调频核心（v7 无状态公式 + 4.6 fg onset，见 §3.2）
│  │  ├─ config.rs              ★ CLG 配置（v7 精简字段，见 §3.2）
│  │  ├─ mod.rs                 ★ 调度线程（亮屏 100ms / 息屏 500ms / 息屏红线，见 §3.9）
│  │  ├─ scheduler.rs / monitor_health.rs
│  │  └─ fas/                   FAS 游戏帧感知调度（PID/帧管线/档位，未改算法）
│  ├─ monitor/
│  │  ├─ app_detect.rs      ★ 前台识别 + FAS 闸门 + inotify 热重载修复
│  │  ├─ cpu_monitor.rs     ★ 自适应采样（亮 100ms / 息 500ms，含单测）
│  │  └─ fps_monitor.rs     ★ eBPF 帧探针懒加载/零空转
│  └─ common.rs             模块根目录注释（路径回溯按 Aegis 命名）
├─ yumi-ebpf/               eBPF 探针程序（未改）
├─ module/                  刷包完整内容（与 prebuilt zip 逐一一致，含二进制）
│  ├─ config/config.yaml    ★ 四模式 CLG 定值（仅大块作用注释，§4.1）
│  ├─ rules.yaml            ★ FAS 定值 + FAS 开关语义（§4.2）
│  ├─ core/bin/Aegis        ★ 4.6 守护二进制（arm64）
│  ├─ webroot/index.html    ★ WebUI 构建产物（xtask 内 npm 从源码构建）
│  ├─ charge_guard/charge_guard.c  charge_guardd v1.7 源码（CI 编译，不进刷机包）
│  ├─ bin/ charge_guard/    充电阈值 + 温控组件（charge_guardd/thermal_manager/busybox）
│  ├─ tools/aegis_adapt.sh  ★ 刷入时一键适配（探测硬件/算权重/动态生成三模式 cluster_ceil）
│  ├─ service.sh / customize.sh / uninstall.sh / module.prop
│  └─ config/i18n/          守护日志中文化
├─ webui/                   Vue WebUI 源码（品牌化 + FAS 开关 + 充电/温控页）
│  └─ src/views/ChargeView.vue  ★ 充电页（含「本次插电锁存」开关，见 §3.6、§3.8）
├─ xtask/                   打包任务（产物名 yumi→Aegis、aegis-<ver>-*.zip）
├─ prebuilt/                ★ 真机验证过的可刷整包（Aegis-scheduling-4.6.zip）
├─ tools/
│  ├─ Aegis_一键适配.sh      ★ 换新设备一键完成：探测硬件/算权重/生成配置/部署/重启
│  └─ aegis_cpu_probe.sh    早期手工硬件探测脚本（一键适配的底层逻辑来源）
└─ docs/
   ├─ Aegis调参说明.txt      ★ 完整参数调参手册（4.6 版，每个参数的影响）
   ├─ monitor-recovery.md   上游文档
   └─ upstream-README*.md   原版 yumi 的 README 备份
```

带 ★ 的是相对原版改动或新增的内容。

---

## 3. 相对原版 yumi 2.0.2 的全部更新

### 3.1 品牌与模块集成（无行为风险的改名）

- 守护二进制 `yumi` → **`Aegis`**：`Cargo.toml` 的 `[[bin]] name`、`xtask` 打包/拷贝路径、`module/.gitignore` 全部同步；包名 `yumi`→`aegis`，描述改为 "Android CPU scheduling daemon (yumi core rebrand)"。
- `module.prop`：id `aegis_power_matrix`、名称「Aegis」、**version `4.6` / versionCode `9`**、作者「默默的阿文」、描述「eBPF 负载调频 · 帧感知调度 · 充电阈值 · 温控」；**删除原 updateJson 在线更新地址**（防止被上游更新覆盖定制）。
- 充电守护 charge_guardd 1.2：电池温度多级回退读取（标准 power_supply 节点 → PMIC VADC IIO `in_temp_bat*therm*` → thermal_zone），修掉部分新 HyperOS 机型（实测 dizi/8Gen3）标准温度节点、dumpsys、thermal_zone **全部恒为 0 桩**导致 WebUI 温度恒显 0°C 的问题；0 值与超出 -20~85°C 的读数一律判无效。标准节点正常的机型（如 8Gen2）走第一级，行为不变。单位按内核 ABI 固定换算（power_supply/dumpsys=0.1°C、thermal_zone=milli°C，仅平台不一的 IIO 用启发式），低温机型不会因单位误判触发高温停充。C 源码不进刷机包（仅在 GitHub 历史/随源码归档），刷入时由 customize.sh 从 `bin/arm64-v8a/` 投放二进制。
- **刷入即适配**：`tools/aegis_adapt.sh` 随包发布，customize.sh 在刷入时自动完成硬件探测与权重生成（KernelSU 真机验证），失败自动回退包内定值，不阻断安装。
- **模块脚本只做三件事（2026-10-15 精简）**：service.sh 只负责启动 Aegis 调度守护 / thermal_manager 温控 / charge_guardd 充电阈值（另保留用户确认的 logd 空转抑制）；uninstall.sh 只回滚这三者（停守护、停充节点还原、删除投放的 thermal conf）；customize.sh 只保留本模块自身升级时的用户设置继承。**已删除**：对 `qcom-battery/fake_temp` 的任何写入、旧 YC/uperf/yumi 模块的停用与继承、resetprop/settings 清理、冻结包恢复等历史遗留动作。
  - 删除原因（真机事故）：dizi（Redmi Pad Pro / PM7250B）内核的 `fake_temp` 语义为"注入温度"，旧 service.sh 的"清理遗留"行 `echo 0 > fake_temp` 在该内核等于**注入 0°C** → 电池 health=Cool（JEITA 冷电池保护）→ 限充约 5W、系统与 SCENE 温度恒 0。实测该节点 -999 为关闭注入（回落真实热敏电阻 ~30°C，health 恢复 Good）；8Gen2 等手机内核写 0 虽无副作用，但统一不再触碰该节点，各机型均保持系统默认。
- `customize.sh`、`config/i18n/zh.ftl|en.ftl`、守护日志文案：全部 Aegis 中文化；`config.yaml` 默认语言改 zh、作者/配置名改 aegis_config。
- `rules.yaml` 字段 `yumi_scheduler` → `aegis_scheduler`，serde 加了 `alias = "yumi_scheduler"`，**旧规则文件直接兼容**。
- WebUI：整体品牌化为 Aegis，首页/应用规则页/日志页中文化重排，新增充电（ChargeView）/温控（ThermalView）页面入口与 shield 图标，FAS 开关与模式切换接入。

### 3.2 CLG 日常调频核心（`src/scheduler/cpu_load_governor.rs`）

**v1→v6 的演进**：低载增益压缩（v5）→ 前台闸门 + 交互态状态机 + 驻留（v6）。v6 解决了滑动起步迟滞，但引入了静态空高（见 §1）。

**v7 重写（基线，无状态）**：

- 删除 `ClusterState.hold` 驻留计数与 onset/sustaining/interactive 全部判定分支；删除"交互态首拍 rate=1.0""交互态禁快降""交互态给满 headroom"三套分叉。
- 删除配置字段 `interact_hold_ticks`、`spike_decay`、`headroom_ramp`（前两者随状态机一起消失，后者在 v6 就已是死字段）。
- 每簇每拍只做：算 raw_util 与 jump → 增益线性插值得 target_perf → 升频（免确认通道 / 普通确认+平滑）或降频（快速沉底 / 正常 / 滞回带慢速）→ 就近取档写频。
- 保留字段：`up/down_threshold`、`low_load_gain`、`headroom_factor`（增益曲线两端）、`spike_jump_threshold` + `interact_fg_util`（jump 通道双条件）、`smoothing_up/down`、`up/down_rate_limit_ticks`、`slow_up/down_scale`、`down_fast_threshold/mult`、`perf_floor/ceil/init`、`up_jump_threshold`。
- 尖峰衰减（`spike_decay`）随交互态一并删除：后台单拍毛刺改由"前台闸门 + up_wait 连续拍确认"双重过滤，前台不忙的单拍 jump 顶不起频率；真实手势的 jump 本来就不该衰减。
- `on_load_update(&core_utils, fg_max_util)` 签名不变（fg 利用率仍为两通道使用）。
- `input_boost_ms` **不属于**这条采样/增益循环：它由独立输入通道驱动（evdev 触摸 → `UserTouch` → `on_input_boost()`），与每拍计算完全解耦，详见 §3.10。
- tick 调试日志计数 50（约 5s 一条）。

**4.6 增补（`fg onset` 免确认通道）**：

```rust
let fg_busy   = fg_max_util >= self.cfg.interact_fg_util;
let idle_onset = fg_busy && cluster.current_perf <= self.cfg.perf_floor + 1e-3;
let touch_jump = fg_busy && (jump >= self.cfg.spike_jump_threshold || idle_onset);
```

- 轻应用从静态起步时，簇 util 跳升常常不到 0.42（如 0.05→0.35），旧逻辑要等 2 拍确认，体感是"第一次滑动/点按慢半拍"；新增通道只在**前台真忙且频率仍贴地板**时成立，目标仍由增益曲线算（只到正确低档），不会过给。
- 后台任务（前台不忙）与息屏（top-app 为空）都不触发，因此不增加任何空转或待机功耗。

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

### 3.4 采样与调度节拍：固定 100ms → 亮/息屏自适应（4.6）

- `monitor/cpu_monitor.rs`：新增纯函数 `cpu_sample_interval(screen_on)`，**亮屏 100ms / 息屏 500ms**；采样循环由 `tokio::interval`（固定 100ms）改为按屏幕状态 `sleep`。息屏时 eBPF map 读取、top-app 扫描、消息投递的次数全部降到 1/5。
  - 单元测试 `sample_interval_follows_screen_state` 锁定 100/500ms 两个值。
- `scheduler/mod.rs`：调度线程 `recv_timeout` 同步改为 **亮屏 100ms / 息屏 500ms**，与采样同拍，避免消息到达后还要等一个固定周期。
- FAS 安全性已核实：`fas/controller.rs` 全部基于真实 elapsed/Instant 计时（util 采样 1000ms、cold_boot_ms、verify 1500ms、帧事件驱动的 freq_force_counter），与 tick 无关；且 FAS 只在亮屏 + fas 应用时激活，不受息屏 500ms 影响。
- 亮屏保持 100ms 的理由：手势起步到首帧只有几十毫秒，100ms 是"跟手"与"开销"的平衡点；息屏没有交互需求，500ms 足以把频率压到底，同时显著减少守护唤醒。

### 3.5 8Gen2 / SM8550 整机定值

见 §4，全部落在 `module/config/config.yaml` 与 `module/rules.yaml`（与 prebuilt 4.6 整包一致）。

### 3.6 WebUI 充电页编辑被弹回修复（`webui/src/views/ChargeView.vue`）

现象：充电阈值页里「安全余量 / 保底电量 / 生效窗口」（以及其他字段）清空数字重新输入时，会被瞬间恢复成旧值。根因：页面每 3 秒轮询磁盘上的 `charge_guard.conf` 并整体回填表单，而旧代码的 `dirty` 标志只在"点击保存"期间置位，**用户编辑时从不置位**——编辑超过 3 秒未保存就被磁盘旧值覆盖（预约区三个字段因为要先开开关、编辑时间最长，最容易撞上）。修复：

- 对 `form` 增加 `flush:'sync'` 深度 watch，用户任意修改立即置 `dirty=true`，轮询回填在 dirty 期间跳过；`fillForm()` 程序化赋值用 `populating` 护栏避免误标脏；保存成功后清 dirty。
- 顺带修正 `sched_margin=0`、`sched_floor=0`（合法值，0 保底=不设保底）被 `parseInt(...) || 默认值` 误吞成 15/20 的问题，改为 `Number.isNaN` 判定。
- 因构建机曾无 Node 环境，prebuilt 内 `webroot/index.html` 一度为手工修补的等价 bundle；**2026-10-17 起构建机已具备 Node 环境，xtask 直接 npm 从 `webui/` 源码构建 webroot**（已修复 Windows 下 xtask 找不到 npm.cmd 的问题），产物与源码完全一致，手工 bundle 作废。
- 4.6 补强：状态显示**先判 `stopped` 再判 `powered`**（主动停充后电池 status 会变成 Not charging，旧顺序会把"已停充"显示成"未充电"），并新增"已锁存"标记（`full_latch=1` 时显示），保存校验与回读校验同时覆盖 `charge_full_latch`。

### 3.7 前台识别重构 + 前台空闲压频 + 按簇封顶（2026-10-17，微信公众号 A/B 真机验证）

- **前台识别改用内核 top-app cpuset**（`monitor/cpu_monitor.rs`）：旧实现只扫前台主进程的线程，微信 WebView/小程序渲染全在子进程，fg 信号漏读（实测公众号页面 fg 中位仅 0.01，jump 通道形同虚设）。现改为读 `/dev/cpuset/top-app/tasks`——内核与 AMS 维护的"前台 App 全进程树"，主进程 + 沙箱渲染 + 小程序进程天然全在内，逐线程取 eBPF runtime 最忙者为 `fg_max_util`；零包名硬编码，全 App 通用，节点缺失自动回退主进程扫描。
- **新增 `background_ceiling`（前台空闲压频）**：前台进程树整体空闲（最忙线程 < `interact_fg_util`）的拍，target_perf 额外封顶。无状态、逐拍独立、前台一忙当拍解除——是"静态延迟压频"诉求的正确形态，未重新引入 v6 式驻留状态机。桌面真·空闲实测咬合 98~100%，三簇沉底（p7=595MHz）。
- **新增 `cluster_ceil`（按簇独立封顶）**：按 policy 升序逐簇覆盖 `perf_ceil`，缺省位回退 `perf_ceil`，normalize 夹在 `[perf_floor, 1.0]`。4.6 起三档都显式给出向量：省电 `[0.70,0.70,0.60]`、标准 `[1.0,1.0,0.85]`、性能 `[1.0×3]`；想单独压超大核只改末位。1/2/3/4 簇机型按向量长度自然映射。
- **一键适配脚本升级**（`module/tools/aegis_adapt.sh`）：按本机簇数动态生成三个模式的 `cluster_ceil` 向量，自动识别超大核槽位（fmax 最高的簇）写入适配建议 TXT；生成校验覆盖 `background_ceiling`、`cluster_ceil`、`charge_full_latch` 等新键。

### 3.8 充电阈值控制 v1.7：充满即停「本次插电锁存」（4.6 核心修复）

**问题现场**：开启「充满即停」后，100% 停充；手机放在床头没拔线，电池自然放电掉到 99%，守护立刻补电回 100%，再停、再掉、再补……整夜反复浮充。用户明确诉求：**识别"没有物理拔电"，当次插电不要再重复充电**。

**根因**：

1. v1.6 的恢复判定是"`charge_full=1` 时只要 `level < 100` 就 `battery_ready = 1`"，天生构成 100↔99 浮充循环；
2. 旧「是否接着电」用 battery status 判断，但**我们主动停充后 status 本身就变成 Not charging / Discharging**，无法区分"拔掉了"和"我们按住了"。

**v1.7 方案**（`module/charge_guard/charge_guard.c`）：

- 新增外接电源检测 `external_power_present()`：遍历 `/sys/class/power_supply`，跳过 battery/bms 与 `type == "Battery"` 的节点，优先读 `online`（VBUS 在位时停充后仍为 1），无 `online` 再看 `present`；返回 1=有外接电源 / 0=确认无 / -1=本机无可用节点（此时退化为连续 3 拍 Discharging 判拔电）。
- 新增会话锁存文件 `charge_guard/data/full_latch`：
  - **写入**：充满停充成功（含低电流涓流判定）即写入；
  - **保持**：只要外接电源仍在，电量掉到 99% 也**绝不补电**（恢复判定里 `full_latched` 直接压住 `battery_ready` 与 `temp_ready`，预约放行分支同样回 `normal_need_stop = 1`）；
  - **解除**：仅当 `ext_power == 0`（物理拔电）→ 清锁存 + 还原充电节点，下次插电照常充满；
  - **安全阀**：电量跌到 `power_start` 以下（默认 70%）自动解锁，避免跨多次插拔或极端场景永久不充电；
  - **迁移**：老版本升级/守护重启时，若"停充原因=充满、电量为 100%"，自动补写锁存，升级当次也不会浮充；总开关关闭、`restore`、关闭 `charge_full`/`charge_full_latch` 都会清理锁存。
- 新增配置项 `charge_full_latch`（默认 **1**）：设为 0 回到 v1.6 的"掉到 99% 立即补电"旧行为，兼容喜欢浮充的用户；WebUI 充电页同步开关与状态提示（`cg_latch`）。

**沙盒仿真验证**（`sim/` 临时工程，逻辑等价于真机 sysfs，验证后未提交）：

| 场景 | 过程 | 结果 |
|---|---|---|
| A 默认锁存 | 100% Full 停充 → 掉到 99%（仍插电）→ 拔电 → 重插到 100% | node=0 latch=1 → 保持 node=0 → 解锁 node=1 → 再次充满停充 |
| B `latch=0` 兼容 | 预置停充 99% 插电 | node 恢复 1（旧行为保留） |
| C 升级迁移 | 预置 100% 停充但无锁存文件 | 自动补写 latch=1，node 保持 0 |

**配套变更**：`charge_guard.conf` 注释、WebUI（表单/校验/文案/状态）、`module.prop` 版本、CI 编译 charge_guardd 源码步骤全部同步；发布包内 `charge_guardd` 二进制与 `charge_guard.c` 严格一致（`-v` 应输出 `charge_guardd v1.7 (Aegis)`）。

### 3.9 4.6 调度无感优化：三种模式重新定值 + 息屏红线 + 起步跟手

目标：**减少守护自身消耗，功能不缩水，做到"官方那种几乎感觉不到额外耗电"**。改动分四层，全部保持无状态：

1. **守护自身少醒**：亮 100ms / 息 500ms（§3.4）；息屏不再做无意义的高频 eBPF 读取与 top-app 扫描；FAS 仍然零空转（§3.3）。
2. **息屏更狠**：`perf_ceil` 与每簇 `cluster_ceil` 统一压到 **0.35**（8Gen2 中核≈1.3GHz / 超大核≈1.5GHz），`background_ceiling` 压到 0.30，`smoothing_up` 0.15、`smoothing_down` 1.0、`slow_up_scale ≤ 0.15`、`down_fast_threshold ≥ 0.30` —— 后台同步/推送不会把大核顶到高频，负载一落立即沉底。
3. **亮屏起步跟手**：新增 `fg onset` 免确认通道（§3.2），配合把 `interact_fg_util` 收到 **0.18**、`spike_jump_threshold` 收到 **0.42（性能 0.32）**，既减少信息流抖动误触发高频，又让真实点按/滑动第一拍就到目标档。
4. **三模式性格拉开**（详见 §4.1）：
   - **省电**：`low_load_gain 0.42`、`perf_ceil 0.70`、超大核 `cluster_ceil 0.60`、`background_ceiling 0.22` —— 静态/看剧压到最深，滑动靠曲线给到钉频流畅线附近；
   - **标准**：`low_load_gain 0.50`、超大核 `cluster_ceil 0.85`、`background_ceiling 0.28` —— 日常默认档，目标紧跟真实负载，超大核只在大负载才动；
   - **性能**：**撤掉 0.10 静态底频**（`perf_floor 0.0`）、`low_load_gain 0.62`、`up_threshold 0.72`、`up_rate_limit_ticks 1`、`down_rate_limit_ticks 4` —— 低门槛快升、缓降但不粘滞，静态不再无谓吃 0.10 底频的那部分功耗。
   - **fast** 依旧锁满频，不参与任何压空转调整。

**为什么这样更"无感"**：省电/标准档的低载增益、息屏红线、自适应节拍三件事同时作用于"没有交互的时间"，这部分才是待机功耗的大头；交互时间则由增益曲线与两条免确认通道保证跟手与帧率。相比 4.3，静态与息屏的功耗路径更短（更少唤醒、更低上限），亮屏滑动路径更快（第一拍到目标），因此"既不卡也不费电"的区间被拓宽，而不是简单地在省电与卡顿之间二选一。

### 3.10 输入抬频（input boost，4.6.1 新增）

**要补的空窗**：CLG 是「100ms 采样 + 跟随已观测负载」变频，手指按下到首帧之间还没采到新负载，频率仍停在地板上，点按→首帧天然多出约 120ms。官方 walt governor 的 input boost 就是补这个信号，这里用 evdev 原始事件复刻，**只补点按瞬间，不改静止/滚动驻留**。

**链路**（`src/monitor/touch_monitor.rs` → `src/scheduler/mod.rs` → `on_input_boost()`）：

- `touch_monitor.rs` 启动时只读打开 `/dev/input/event*`（不 `EVIOCGRAB`，不干扰系统输入分发），`poll` 读原始 `input_event`，只认 `BTN_TOUCH`（value=1）与 `ABS_MT_TRACKING_ID >= 0` 两种"落下"信号，键盘/侧键/传感器一律忽略；300ms 去抖后发 `DaemonEvent::UserTouch`。
- `scheduler/mod.rs` 收到事件后，仅当**亮屏 + 当前模式非 fas + CLG 已接管 + 事件延迟 <200ms** 时调用 `CpuLoadGovernor::on_input_boost()`。
- `on_input_boost()` 把各簇下限抬到 **0.85 档**（再按 `cluster_ceil` 夹紧；8Gen2 实测 p0=1785600 / p3=2457600 / p7=2342400），min/max 同步写成同一频率钉住，并保持 `input_boost_ms`（默认 150ms，**0 = 关闭**）；窗口结束后交回正常降频路径自然回落，不引入任何驻留状态。
- 抬频窗口内的每一拍**绕过 `background_ceiling`**：点按瞬间前台线程还没跑起来、簇 util 必然很低，若仍按"前台空闲"封顶，会把刚抬起的频率当拍打回。

**三个限制（都踩过，改配置/写自动化时要记住）**：

1. **改配置要就地重写 + 息屏再亮屏**：`config/config.yaml` 的监视只认 `IN_CLOSE_WRITE`，`sed -i`（临时文件 + rename）不触发；且 `input_boost_ms` 属于 CLG 配置，只在 `reload_config()` 被调用时才生效（屏幕状态变化 / 模式切换 / `rules.yaml` 重载 / 守护启动）。正确做法：`cat > config.yaml` 就地写，再息屏→亮屏一次。
2. **`input tap` 不触发抬频**：它走 Android InputManager 注入，不产生 evdev 原始事件；只有真实手指触摸才会发 `UserTouch`，自动化脚本测不到这条通道。
3. **触摸设备只在启动时枚举一次**：`start_touch_loop` 启动时扫一遍 `/dev/input/event*` 后不再重扫，热插拔/新增输入设备要重启守护才会被监听。

**实测收益**（MIX Fold 3，亮屏标准档，对照 4.6 无抬频）：

| 场景 | 指标 | 4.6 | 4.6.1 |
|---|---|---|---|
| Recents 点按 | 到首帧 p50 | 239ms | **123ms** |
| Recents 点按 | 顿挫次数 | 9/10 | **0/10** |
| 下拉通知栏 | 起手最坏帧 | 75ms | **31ms** |
| 看小说翻页 | 折算耗电 | — | **<0.03%/小时** |

连续滑动不会叠加抬频：`on_input_boost()` 只刷新窗口、不叠档位，滚动期间频率本来由真实负载维持，实测滑动功耗与不开抬频同量级。

---

## 4. 8Gen2 定制参数一览

### 4.1 CLG 四模式（`module/config/config.yaml`，v7 内核 + 4.6 / 4.6.1 定值）

| 参数 | powersave | balance | performance | fast |
|---|---|---|---|---|
| up_threshold | 0.80 | 0.80 | **0.72** | 0.01（锁满） |
| down_threshold | 0.50 | 0.48 | **0.42** | 0.01 |
| smoothing_up / down | 0.55 / 0.45 | 0.70 / 0.35 | 0.85 / 0.22 | 1.0 / 0.01 |
| up / down rate(ticks) | 2 / 2 | 2 / 2 | **1 / 4** | 1 / 10 |
| headroom_factor（增益高端） | 1.05 | 1.05 | **1.10** | 2.0 |
| perf_floor / ceil / init | 0.0 / **0.70** / 0.22 | 0.0 / 1.0 / 0.32 | **0.0** / 1.0 / 0.45 | 1.0/1.0/1.0 |
| **low_load_gain（增益低端）** | **0.42** | **0.50** | **0.62** | 1.0 |
| **spike_jump_threshold** | **0.42** | **0.42** | **0.32** | 0.35 |
| **interact_fg_util（双通道闸门）** | **0.18** | **0.18** | **0.18** | 0.25 |
| up_jump / slow_up / slow_down | 0.35 / **0.15** / **0.55** | 0.35 / **0.18** / **0.55** | 0.35 / **0.20** / **0.40** | — |
| down_fast_thresh / mult | **0.25** / 4.5 | **0.08** / 4.0 | **0.18** / 3.0 | 0.10 / 2.5 |
| **input_boost_ms（点按抬频，0=关闭）** | 150（未显式，继承默认） | **150**（YAML 显式） | 150（继承默认） | 150（继承默认；锁满频，无实际意义） |
| **background_ceiling（前台空闲封顶）** | **0.22** | **0.28** | **0.38** | —（不参与） |
| **cluster_ceil（按簇封顶，policy 升序）** | **[0.70, 0.70, 0.60]** | **[1.0, 1.0, 0.85]** | **[1.0, 1.0, 1.0]** | —（不参与） |

三档性格：**省电档**压得最深（gain 0.42 + 中核/小核 0.70 封顶 + 超大核单独 0.60），静态与看剧几乎不碰超大核；**标准档**增益略浅、超大核留 15% 余量（0.85）只在大负载才动，是日用默认档；**性能档**撤掉旧的 0.10 静态底频、`up_threshold 0.72` + `up_rate_limit_ticks 1` 低门槛快升，降频仍然立即（`down_rate_limit 4`、`down_fast 0.18`），避免"性能档反而费电"。

**fast 锁满档不参与任何压空转调整。**

**息屏红线（运行时，非 YAML 定值）**：息屏后对当前模式的配置再做一次上限收敛 —— `perf_ceil ≤ 0.35`、每簇 `cluster_ceil ≤ 0.35`、`background_ceiling ≤ 0.30`、`smoothing_up = 0.15`、`smoothing_down = 1.0`、`slow_up_scale ≤ 0.15`、`down_fast_threshold ≥ 0.30`，亮屏后恢复当前模式原值。这条红线与 §4.1 的表格叠加生效，想调夜间功耗只需要改 `scheduler/mod.rs` 的 0.35/0.30 两个数字。

v3 时期的理论换算仍可作为手感参考（中核 499~2803MHz）：`util 0.20 × 0.42 → ~683MHz`、`util 0.42 × 0.42 → ~886MHz`、`util 0.70 → 约 1.6GHz`、真重载顶 `cluster/perf ceil`。4.6 把低载增益再压 0.02~0.05，静态与视频播放更省，滑动仍由 §5.3 的钉频流畅线兜底。

> 两个 YAML 只保留每个大块上方一行作用注释（无行内解释、无历史调参记录，避免换设备时编码乱码与陈旧误导），完整参数含义以本文档与 `docs/Aegis调参说明.txt` 为准。

> **安装会重置配置**：刷入时 `customize.sh` 会调用 `tools/aegis_adapt.sh` 按本机硬件重新生成 `config/config.yaml`（`rules.yaml` 只替换 `fas_rules` 段、充电/温控配置不动），你在模块目录里手改过的阈值会被这份新生成的默认值覆盖。所以**装完必须核对**：标准档是否为 `down_fast_threshold: 0.08` / `down_fast_mult: 4.0`、`input_boost_ms` 是否存在（4.6.1 的 `-fixed` 包已修正生成模板，装完即为此值；旧的 `aegis-4.6.1-44-20261106-0831.zip` 仍会写成 0.25 且无该字段）：
>
> ```bash
> adb shell 'su -c "grep -n -E \"down_fast_threshold|down_fast_mult|input_boost_ms\" /data/adb/modules/aegis_power_matrix/config/config.yaml"'
> ```
>
> 若被旧模板写成 0.25，用备份还原后再**就地重写 + 息屏再亮屏**使其生效（见 §3.10）。

### 4.2 FAS 游戏档（`module/rules.yaml`，要点）

- 三簇算力权重（`auto_capacity_weight: false` 显式固化，换 ROM 不漂）：**1.00 / 2.43 / 2.69**，由 capacity 266/811/1024 按 `1+√(cap/266−1)` 换算；
- perf 范围按三簇频率表换算：`perf_floor 0.30`（小≈820/中≈1190/超大≈1425，游戏进场不卡）、`perf_init 0.50`（≈1162/1651/1978）、ceil 1.0、cold_boot 0.85；
- PID kp/ki/kd = 0.05/0.01/0.006（上游默认，未动）；
- 游戏帧率档/加载保护/快速衰减等帧计数参数保持上游默认；
- `global_mode: "balance"`、`fas_enabled: false`（默认零探针，仅指定的游戏唤醒 FAS）。

### 4.3 硬件频率表（perf 归一化依据）

| policy | 核心 | 频点范围 | 档位 | capacity |
|---|---|---|---|---|
| policy0 | cpu0-2 · 3×A510 小核 | 307200~2016000 kHz | 16 | 266 |
| policy3 | cpu3-6 · 2×A715+2×A710 中核 | 499200~2803200 kHz | 19 | 811 |
| policy7 | cpu7 · 1×X3 超大核 | 595200~3360000 kHz | 20 | 1024 |

8Gen2 标准版（3.2GHz）与领先版（3.36GHz）capacity 表与调频机制相同，配置 AB/AC 通刷，perf 归一化后自动落各自真实最高频。

---

## 5. 真机实测与验证

> §5.1/5.2 测于 v7 首版参数，是 v7 公式与钉频流畅线的依据；**§5.3 是 v3 参数的三档真机复核（2026-10-15 晚）**；§5.5 是 4.3 的前台识别 A/B。4.6 的改动（充电锁存、自适应采样、fg onset、息屏红线、三模式定值）以**逻辑仿真 + 单元测试 + 参数一致性校验**为主，详见 §5.6；真机功耗/帧率请按 §5.6 的方法复测后更新本节。钉频流畅线是物理结论，与参数版本无关。

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

**MT 管理器（bin.mt.plus，轻量文件列表）**：

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

| 指标 | v7 原参 | v7 省电档定参 |
|---|---|---|
| p3 中位 | 2054 | **1920**（=钉频流畅线） |
| 顶格 2188 占比 | 24% | **16%** |
| 大核 ≥2GHz 占比 | 7% | **3%** |
| GFX Janky / Missed Vsync | 0 / 0 | **0 / 0** |
| 95 / 99 分位 | 17 / 24 ms | **17 / 22 ms** |

调参方法备注：先录制逐拍簇 util/前台线程 util 轨迹，本地按 governor 公式回放扫参数，再只把候选档上真机 A/B（含 GFX）；曾试中位 1786 的更激进档，零丢帧但 95 分位升到 18ms、legacy 卡顿率明显上涨，低于钉频流畅线，弃用。

### 5.3 v3 三档空转复核（2026-10-15 晚，4.3 包内参数）

方法：一键适配 SH 部署最终配置后，以 200ms 间隔采三簇 `scaling_cur_freq` + `/proc/stat` 占用，每场景 25~26s，**剔除前 3 秒启动期**只看稳态。红果为无操作稳态播放；酷安为每 1.8s 注入手势的连续滑动（负载比真人重——真人两滑之间有停顿，停顿即走快速沉底，日用数据只会更低）。

**红果短剧稳态播放**（p0 小核 / p3 中核 / p7 超大核）：

| 档位 | p0 中位 / 占用 | p3 中位 / p90 | p3 ≥1.5G | p7 中位 / ≥2G | p7 占用 |
|---|---|---|---|---|---|
| 省电 | 1344 / 73% | **845 / 1286** | 4% | 595 / 1% | 6.6% |
| 标准 | 1344 / 68% | **730 / 1171** | 1% | 595 / 4% | 7.5% |
| 性能 | 1670 / 62% | 1171 / 1651 | 12% | 864 / 0% | 1.3% |

**酷安连续滑动**：

| 档位 | p3 中位 / p90 / 峰值 | p3 ≥1.5G | p7 中位 / ≥2G |
|---|---|---|---|
| 省电 | **1651 / 1920 / 2054**（峰值=ceil 0.70 封顶线） | 86% | 595 / 1% |
| 标准 | 1786 / 2189 / 2458 | 94% | 595 / 4% |
| 性能 | 2189 / 2458 / 2707 | 96% | 864 / 0% |

结论：

- **没有空转虚高**——高频段全部对得上真实占用：红果稳态省电档中核 845MHz（理论推算 886，吻合）、超大核基本睡死（占用 6.6%）；小核 1.3GHz 时占用 60~73%，是解码/音频/网络真忙。
- 酷安省电档 **p90=1920MHz 正贴 §5.1 钉频流畅线**，峰值被 ceil 0.70 封在 2054，p7≥2GHz 仅 1%——给频精准，没过给；标准/性能按档位性格依次放开。
- 性能档中核 2.2GHz 时占用约 30%，是"快升缓降"的设计行为（主动切性能档才吃到），不是 bug；4.6 已撤掉该档的 0.10 静态底频，纯静态时不再无谓升高。
- 同次测试真机验证了一键适配 SH：探测 3 簇/权重算成 1.00·2.43·2.69（与定值一致）/备份/覆盖/重启守护全链路通过（首跑暴露的校验计数 bug 已修复）。

### 5.4 稳定性

- 守护连续运行零 panic（`grep -c panic logs/daemon.log` = 0）；
- 静态桌面 p3 贴 499~614，打字/微信消息场景主观可接受（用户确认）。

### 5.5 top-app 前台识别 + 前台空闲压频 A/B（2026-10-17，微信公众号文章页）

同一自动化场景（10s 静止 → 10 次慢滑 → 10s 停顿 → 6 次慢滑，`dumpsys gfxinfo` 计帧，100ms 逐拍日志统计）：

| 指标 | 修复前（主进程 fg，闸门漏读） | 修复后（top-app 全进程树） |
|---|---|---|
| 慢滑 X3 中位 | 2227MHz（56% ≥2G） | **864MHz（53% ≤902M）** |
| 松手后 X3 | 1594MHz 粘滞 | **95% 秒沉底 595M** |
| 静止中核 | 中位 1536MHz（88% ≥1.5G） | **614MHz，0% ≥1.5G** |
| 流畅度 | — | **jank 0.02%、p99=7ms（120fps 预算内）** |
| 桌面真·空闲 | — | fg=0.00，background_ceiling 咬合 98~100%，三簇沉底 |

交叉验证：文章页 fg 读数 0.55~0.63 与 `top -H` 实测渲染线程占用（XWeb CrRendererMain 51.7% + Compositor 14%）吻合——微信 XWeb 渲染线程在页面静止时仍持续烧 50~60% 一核，属 App 自身行为，调度器跟随真实负载给频正确；重动画文章慢滑 2.2GHz 对应 p99=7ms，贴合 120fps 需求，非过给。

### 5.6 4.6 验证记录与复测建议

已完成的验证（均为本次实际执行）：

- **充电锁存**：沙盒仿真三场景（默认锁存 / `latch=0` 兼容 / 升级迁移）全部符合预期，过程见 §3.8 表；`charge_guard.c` 用 NDK `aarch64-linux-android21-clang -fsyntax-only -Wall -Wextra` 检查零警告零错误。
- **采样单测**：`cpu_sample_interval(true)=100ms`、`(false)=500ms`（`sample_interval_follows_screen_state`）。该测试目标依赖 Linux 专有性（inotify/logd），由 CI 的 `cargo test -p aegis --bin Aegis --locked` 在 Ubuntu 上执行；Windows 主机只做 arm64 交叉编译验证。
- **参数一致性**：`module/config/config.yaml` 与 `module/tools/aegis_adapt.sh` 生成的三模式定值逐项一致（避免"刷入即适配"把新参数覆盖回旧值）。

真机复测建议（4.6 未重新做整机功耗/帧率实验，请按下面方法记录后再更新本节）：

1. **夜间待机**：充满 100% 后不拔线睡一晚，次日看 WebUI 是否显示 `已锁存`、电量是否停在 100% 或 99% 且**没有新的充电记录**；拔线再插一次，确认恢复充电。
2. **息屏功耗**：`adb shell dumpsys batterystats --charged` 或 Scene 记录 8h 息屏耗电，对比 4.3；同时 `logcat` 观察 Aegis 唤醒次数应约为 4.3 的 1/5。
3. **亮屏手感**：酷安/微信文章页各跑 §5.5 场景，确认首次滑动起步不再慢半拍（fg onset），且 jank 不高于 4.3。
4. **三档对比**：省电档看剧、标准档日常、性能档重载，各记录 p3 中位/p90 与 GFX jank，与 §5.3 表对照偏差应在 ±10% 内。

---

## 6. 编译方法

Windows 主机交叉编译（本机已验证环境，2026-10-17）：

```powershell
$env:RUSTUP_HOME="D:\dev\rustup"; $env:CARGO_HOME="D:\dev\cargo"
$env:ANDROID_NDK_HOME="D:\dev\ndk\android-ndk-r29"
$env:RUSTFLAGS="-C default-linker-libraries"
# 仓库在含中文的路径（如 OneDrive\文档）时，MinGW dlltool 打不开 target 里的临时 def 文件，
# 必须把构建目录指到纯 ASCII 路径（xtask 已支持 CARGO_TARGET_DIR）
$env:CARGO_TARGET_DIR="D:\dev\target-aegis"
# MinGW-w64 完整 bin 必须在 PATH 且位于 rustup self-contained 之前（提供 as.exe/dlltool，踩坑结论）
$env:PATH="D:\dev\mingw64\bin;D:\dev\bpflinker;D:\dev\cargo\bin;" + $env:PATH

cargo xtask build   # 一条命令：WebUI(npm) + Rust 核心 + 整包 zip → output\aegis-4.6-<git>-<时间>.zip
# 只编守护：cargo +nightly ndk --platform 26 -t arm64-v8a build -Z build-std -r
```

- 产物：`target/aarch64-linux-android/release/Aegis`；整包 `output/aegis-4.6-*.zip`（**产物名与 Release 版本号统一取自 `module/module.prop` 的 `version`**；`Cargo.toml` 的 `4.6.0` 只是合法 semver，不出现在文件名里）
- 工具链：nightly（x86_64-pc-windows-gnu，rustup set default-host）+ `cargo-ndk` + Android NDK r29（platform 26 = Android 8.0）
- WebUI 由 xtask 自动 `npm run build`（Windows 已修 npm.cmd 问题）后收进 webroot，无需手动构建
- charge_guardd 由 CI / 本地用 NDK 静态编译（源码 `module/charge_guard/charge_guard.c`）：
  ```powershell
  # 以 NDK r29 为例，产物投放两处（模块根 + bin/arm64-v8a）
  & "$env:ANDROID_NDK_HOME\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android21-clang.exe" `
      -static -O2 -o module\charge_guardd module\charge_guard\charge_guard.c
  ```
- 整包打包：优先用 xtask（产出 `aegis-4.6-*.zip`）；手工打包时**禁用 PowerShell 的 Compress-Archive**（反斜杠条目在 Android 上乱），用：
  ```powershell
  $names = Get-ChildItem -LiteralPath <包目录> -Force | ForEach-Object Name
  tar.exe -a -cf out.zip -C <包目录> @names
  ```
- 发布：推 `main` 触发 Pre-release 构建校验；推 tag **`V4.6`** 触发正式 Release（工作流会校验 `module.prop` 的 `version`/`versionCode` 与 tag 一致）。**Releases 从 4.6 起连续编号。**

prebuilt 里的整包 = 调度模块 + 充电阈值/温控脚本与二进制（charge_guard、thermal_manager、busybox 等）合并后的版本，结构以整包为准。

## 7. 刷入与热部署

- **正常刷入**：KernelSU 管理器从 zip 安装，重启。`customize.sh` 会保留已有的模式/FAS 开关/应用规则与充电阈值配置（含 `charge_full_latch`）。命令行：
  ```sh
  /data/adb/ksu/bin/ksud module install /sdcard/Aegis-scheduling-4.6.zip
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
- 充电守护：`charge_guardd -v` 应输出 `v1.7`；状态文件 `charge_guard/data/status.kv` 里 `full_latch=1` 表示"本次插电已充满并锁存"；需要立即恢复充电可 `charge_guardd restore`（会一并清理锁存）。

## 8. 调参手册

完整参数说明见 [docs/Aegis调参说明.txt](docs/Aegis调参说明.txt)（含 perf↔频率换算、每个参数调高/调低的影响、推荐范围与调参纪律）。4.6 最常动的四个：

- `low_load_gain`：增益曲线低端，静态压频力度。当前省电 0.42 / 标准 0.50 / 性能 0.62；觉得安装/计算变慢 → 各 +0.03~0.05（省电不要低于 0.40）。
- `spike_jump_threshold`：手势起步当拍升频的跳变门槛。当前省电/标准 0.42、性能 0.32；若信息流抖动仍偶发顶高频 → 0.45；若慢拖开头肉 → 0.40（4.6 已有 fg onset 兜底，通常不需要再降）。
- `background_ceiling`：前台树整体空闲时的额外封顶。当前 0.22 / 0.28 / 0.38；看剧还想更省 → 省电档 0.20。
- `cluster_ceil`：按簇封顶向量（policy 升序）。当前省电 `[0.70,0.70,0.60]`、标准 `[1.0,1.0,0.85]`；只想压超大核就把末位再降 0.05。

息屏相关只想调夜间功耗：改 `src/scheduler/mod.rs` 的 `0.35`（上限红线）与 `0.30`（前台空闲封顶）；改采样节拍：改 `src/monitor/cpu_monitor.rs` 的 `SAMPLE_MS_SCREEN_ON/OFF`（注意单测同步）。

原则：**流畅靠「低地板 + 连续增益曲线 + 免确认起步 + 快速沉底」，不要抬 perf_floor，也不要重新引入驻留/状态补丁**。怀疑某场景给频不够时，先钉频 + GFX 量出真实流畅线（见 §5.1 酷安实验），再反推增益。

## 9. 换设备移植流程

**刷入即适配（默认，无需任何操作）**：整包内置 [`module/tools/aegis_adapt.sh`](module/tools/aegis_adapt.sh)，刷入时 customize.sh 自动：探测分簇/policy/频率/capacity → 按内核同公式算权重 → 生成 rules.yaml/config.yaml；升级时保留用户的模式/FAS 开关/每应用规则，只替换 fas_rules 硬件段。任何探测失败都回退包内定值，绝不阻断刷入。2026-10-15 已在 KernelSU 真机验证（dizi/8Gen3：2 簇识别、权重 1.00/2.15 自动算对、旧规则正常合并）；2026-10-17 起适配脚本还会**按本机簇数动态生成三个模式的 cluster_ceil 向量并识别超大核槽位**（babylon 3 簇 install 模式验证：向量 3 槽、值正确、校验全过）；4.6 起生成的三模式定值与 §4.1 完全一致。

**手动方式（适配逻辑改动或不想重刷时）**：刷入后把 [`tools/Aegis_一键适配.sh`](tools/Aegis_一键适配.sh)（与包内 aegis_adapt.sh 同内容）用 MT 管理器以 Root 执行：探测 → 生成 → 备份旧配置（.bak.时间戳）→ 覆盖模块目录 → 重启守护 → 输出 `/sdcard/Aegis_适配建议.txt`（含 cluster_ceil 槽位与超大核位置提示）。曾于 MIX Fold 3 真机验证全链路（3 簇识别、权重 1.00/2.43/2.69、守护正常重启）。CLG 的阈值/增益类参数跨 SoC 通用，无需手工换算。

手工流程（脚本逻辑来源）：

1. 跑 [`tools/aegis_cpu_probe.sh`](tools/aegis_cpu_probe.sh) 拿到分簇、每簇频率表、每核 cpu_capacity。
2. 按 `权重 = 1 + √(本簇cap/最小簇cap − 1)` 算各簇权重，写入 `rules.yaml` 的 `cluster_profiles` 并置 `auto_capacity_weight: false`。
3. perf↔频率换算：`频率 = 最低频 + perf×(最高频−最低频)`，据此重定 FAS 的 perf_floor/init 与四模式 perf_init。
4. 刷入后用 `dumpsys gfxinfo <pkg> reset` + 滑动验证起步延迟与 jank；重负载信息流按 §5.1 钉频量一次真实流畅线，再按 §8 微调到主观手感。

## 10. 设计红线与注意事项

- 温控阈值、充电阈值（charge_guard 配置）**绝不因调度问题改动**；它们由模块族独立组件管理。
- **充电锁存只读供电节点的 `online`/`present`**，绝不能用 battery status 判断插拔（主动停充后 status 必然是 Not charging/Discharging）；锁存必须保留"电量低于 `power_start` 自动解锁"的安全阀。
- 静态省电是第一诉求（小核 800MHz 级别），修丢帧不得靠抬高静态 floor；性能档也不设静态底频（`perf_floor 0.0`）。
- `background_ceiling` / `cluster_ceil` / 息屏红线只做**逐拍无状态上限**，禁止重新引入驻留/状态机（v6 教训）；前台识别保持内核 top-app 通用信号，**不得引入包名白名单**。
- 免确认通道只允许"前台真忙 + 贴地板或真实大跳变"两类条件，目标必须仍由增益曲线算出，禁止"首拍拉满"。
- perf 写频是 min=max 钉频（performance governor）：升频先 max 后 min，降频先 min 后 max；取档 `find_nearest_freq` 就近。
- 采样节拍改动必须同时改 CPU 采样与调度线程超时（保持同拍），并同步 `sample_interval_follows_screen_state` 单测。
- 低增益压频的已知代价：App 在前台做非触摸重活（JS 计算、安装）且前台线程 util 低于闸门、又没有大跳变时会被压频——无帧延迟约束，可接受。
- 本仓库为私人使用，不处理社群 Issue；上游许可证 GPL-3.0-or-later 继续生效，原作者 yuki。
