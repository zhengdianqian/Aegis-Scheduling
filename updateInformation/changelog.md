# 🚀 更新日志 | Changelog

## 2

* **[严重修复]** 删除 service.sh/uninstall.sh 中对 `qcom-battery/fake_temp` 的写入：该"清理遗留"动作在 PM7250B 等新内核（实测 dizi/Redmi Pad Pro）上等于注入 0°C，导致系统温度恒 0、JEITA 冷电池保护限充约 5W。模块脚本精简为只做调度/充电阈值/温控三件事，不再改动任何其他系统状态
* **[充电]** charge_guardd 1.2：温度读取多级回退（标准节点 → PMIC VADC IIO → thermal_zone）+ 物理合理性校验；单位按内核 ABI 固定换算，避免低温机型（0.1°C 原始值 ≤100）被误判高温停充；标准节点正常的机型行为不变
* **[安装]** 刷机包不再夹带 C 源码与冗余二进制（刷入时从 bin/arm64-v8a 投放）；保留 logd 空转抑制
* **[安装]** 刷入即适配：customize.sh 自动探测本机 CPU 分簇并生成算力权重/perf 范围，升级保留用户规则，失败自动回退包内定值
* **[版本]** 版本号统一为 2（version/versionCode），作者：默默的阿文

## 📦 v2.0.2（上游基础版本历史记录）

### 🎯 新增 (New Features)
* **[CLG]** 新增 8 个可调参数，全面开放调频行为定制：
  - `headroom_ramp`：headroom 在 up_threshold 附近的过渡带宽度（默认 0.15）
  - `up_jump_threshold`：快速升频通道的跳变幅度阈值（默认 0.35）
  - `slow_up_scale`：滞回带内升频的最低速率基准（默认 0.02）
  - `slow_down_scale`：滞回带内降频的缩放系数（默认 0.5）
  - `down_fast_threshold` / `down_fast_mult`：极低负载快速降频的触发阈值与放大倍数
  - `spike_jump_threshold` / `spike_decay`：单 tick 尖峰抑制的跳变阈值与衰减比例

### ⚡️ 优化与特性 (Optimizations & Features)
* **[CLG]** CPU 负载调速器全面重构：
  - **降频不再锁死高位**：滞回带内（down_threshold ~ up_threshold）目标低于当前即可降频，按慢速/正常/快速三档平滑回落
  - **headroom 平滑过渡**：在 up_threshold 附近线性渐变，消除负载临界时的频率振荡
  - **中等负载升频提速**：滞回带内升频速率随 util 接近 up_threshold 线性提升，负载回升时不再以 0.008/tick 缓慢爬升
  - **尖峰抑制**：单 tick 负载跳升超过阈值时衰减其增量，孤立瞬时尖峰（如单核 0↔100%）不再瞬间拉满性能；持续负载下一 tick 即全量生效
  - **降频加速**：极低负载（< down_fast_threshold）跳过降频确认期立即快速回落，消除尖峰消失后频率悬停高位
  - **热重载优先**：模式切换/息屏亮屏仅热重载配置，不再全量重建 sysfs writer
* **[AppDetect]** 同模式 ModeChange 去重：配置重载/亮屏恢复不再产生 `balance -> balance` 冗余事件

### 🐛 修复 (Bug Fixes)
* **[CLG]** 修复配置段不生效：`Mode` 的 `PascalCase` 反序列化与 YAML `cpu_load_governor` 键不匹配，用户配置（含 `enabled`）此前从未生效，现全部正确应用
* **[CLG]** 修复 `perf_floor > perf_ceil` 或 NaN/±Inf 配置导致的 `f32::clamp` panic（配置自动规范化并回退默认值）
* **[CLG]** 修复 `release()` 后不恢复 governor/频率：新增接管前状态快照，退出时按序恢复；读取失败不写退化值、恢复失败自动保留重试
* **[Scheduler]** 修复 IPC 线程 panic 后静默死亡：事件循环包 `catch_unwind`，panic 可见并自动释放 CPU 控制权；config watcher 失败增加退避，消除忙循环
* **[Scheduler]** 修复未知/空模式名意外启用 CLG（现默认禁用，不再用默认参数接管 CPU）
* **[i18n]** 修复非法 language 标签导致启动早期 panic（自动回退 en）
* **[FAS]** 修复配置级 clamp panic：perf floor 超过低 ceil、fast_decay 步长边界反转等场景全部规范化
* **[FAS]** 修复 floor-rescue 自救失效：死锁救援不再被 max_inc 截断，可真正跳出 perf_floor
* **[FAS]** 修复 PID 系数被非法 target_fps（0/负/NaN）污染；per-app 帧率档位过滤非法值

## 📦 v2.0.1

### 🎯 新增 (New Features)
* **[FPS 监控]** eBPF FPS 探针重构为单实例多 PID attach 架构，PID 切换零延迟、零丢帧

### ⚡️ 优化与特性 (Optimizations & Features)
* **[CLG]** CPU 负载调速器升频阻尼优化：
  - 新增 `up_rate_limit_ticks` 升频速率限制（连续 N tick 高负载才升频，默认 2）
  - 跳变阈值从 0.20 提高到 0.35，减少瞬时毛刺响应
  - 小幅 creep 系数 0.05 → 0.02，低负载波动几乎不升频
  - headroom 仅在 util ≥ up_threshold 时生效，低负载不放大
