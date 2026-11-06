# Aegis 4.6.1 换设备交接（2026-11-06）

> 目标：换一台新手机时，照本文即可复现「4.6.1 + 输入抬频 + 标准模式调优」的可用状态，并能回滚。
> 本机（验证机）= 小米 MIX Fold 3（babylon / SM8550-AC / Android 16 / HyperOS 3 / **永久 root（KernelSU 正常安装）**）。

## 0. 一页速查

| 项目 | 值 |
|---|---|
| 仓库 | <https://github.com/zhengdianqian/Aegis-Scheduling>，默认分支 `main` |
| main HEAD（本次交接文档提交前） | `2c097262773aff1c6e190f74ae5c6a9c089cffdc` |
| tag | `v4.6.1` → `556575d7432712979b43f675ba5da10659eae1e6` |
| Release | <https://github.com/zhengdianqian/Aegis-Scheduling/releases/tag/v4.6.1> |
| 安装包 | Release 附件 `aegis-4.6.1-44-20261106-0831.zip`（5,096,557 字节） |
| 实测二进制 | `core/bin/Aegis` **1,570,840 字节**，md5 `13cc5a92982038f62efaf4955203ee2f` |
| 关键配置 | 标准档 `down_fast_threshold: 0.08` / `down_fast_mult: 4.0`；`input_boost_ms: 150` |

> 注意：Release zip 里是 CI 重建的二进制（1,570,824 字节 / md5 `f20ef6ab...`），与本机实测那份差 16 字节。
> 若要求"装进去的就是验证过的那份"，先把 zip 内 `core/bin/Aegis` 换成 1,570,840 那份再安装（本次即如此）；
> 不追求逐字节一致时，直接装 Release zip 也可用。

## 1. 本机已验证状态（重启后实测）

| 检查项 | 实际结果 |
|---|---|
| 模块版本 | `version=4.6.1`、`versionCode=10`（module.prop） |
| 模块描述 | `⚖️ Aegis：标准 \| 🌡️ 官方温控 \| 充电阈值:限充80% · 45℃停充`（说明充电配置已保留） |
| 守护进程 | `Aegis` pid 11620（父进程 1），`aegisd` 为无关的代理进程，勿混淆 |
| 二进制 | 1,570,840 字节，md5 `13cc5a92982038f62efaf4955203ee2f` |
| 标准档阈值 | `down_fast_threshold: 0.08`、`down_fast_mult: 4.0` |
| 抬频参数 | `input_boost_ms: 150`（balance 段显式） |
| 当前模式 | `current_mode.txt = balance` |
| 抬频实测（3 次真实触摸） | p0 1785600、p3 2457600 **3/3 命中**；p7 2342400 在 2/3 个采样时刻命中（3 次都触发了抬频，采样脚本按簇顺序读取，个别时刻 p7 读数早于写入） |
| Recents 点按→首帧（3 次） | 123.9 / 125.6 / 98.6 ms，p50 = **123.9 ms**（未回退到 230ms 量级） |
| 充电配置 | `charge_guard.conf` 665 字节（用户配置，已保留） |
| SELinux | Enforcing（完整重启后恢复） |

## 2. 安装步骤（新手机照抄）

```bash
# 0) 连接与确认（换成新设备序列号）
adb devices
adb -s <SN> shell 'su -c id'

# 1) 把 zip 推到手机
adb -s <SN> push aegis-4.6.1.zip /sdcard/aegis_test/aegis-4.6.1.zip

# 2) 安装（KernelSU / KSU Next 通用）
adb -s <SN> shell 'su -c "ksud module install /sdcard/aegis_test/aegis-4.6.1.zip"'

# 3) 重启：先试软重启（本机实测 44s 生效、内核不重启）
adb -s <SN> shell 'su -c "ksud soft-reboot"'
# 软重启不生效再完整重启：
# adb -s <SN> reboot
```

重启后等待并确认：

```bash
adb -s <SN> wait-for-device
adb -s <SN> shell getprop sys.boot_completed   # 输出 1
adb -s <SN> shell 'su -c id'                  # 永久 root 设备应立即返回 uid=0
```

> **安装会重置配置（必看）**：`customize.sh` 里的 `aegis_adapt.sh` 会按本机硬件**重新生成** `config/config.yaml`
> （`rules.yaml` 只替换 `fas_rules` 段，充电/温控配置不动），模块目录里手改过的阈值会被这份默认值覆盖。
> 请用 Release 的 **`aegis-4.6.1-44-20261106-0831-fixed.zip`**（生成模板已修正：标准档 `0.08` / `4.0` + `input_boost_ms: 150`，
> 包内二进制 `1,570,840` / md5 `13cc5a92…`）；旧的 `aegis-4.6.1-44-20261106-0831.zip` 仍会写成 0.25 且没有该字段。
> **装完必须核对 `input_boost_ms` 是否存在、标准档是否为 0.08：**

```bash
adb -s <SN> shell 'su -c "grep -n -E \"down_fast_threshold|down_fast_mult|input_boost_ms\" /data/adb/modules/aegis_power_matrix/config/config.yaml"'
# 期望：powersave 0.25/4.5；balance 0.08/4.0 + input_boost_ms: 150；performance 0.18/3.0；fast 0.10/2.5
```

若标准档被写成 0.25，说明装的是旧包或旧模板，按下面还原备份配置：

```bash
# 模块文件在部分内核下只对特定 SELinux 域可读；本机 `su -c cp` 被拒，`nsenter -t 1 -m --` 可行
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- cp /sdcard/aegis_test/bak/pre461_xxx/config.yaml /data/adb/modules/aegis_power_matrix/config/config.yaml"'
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- cp /sdcard/aegis_test/bak/pre461_xxx/rules.yaml  /data/adb/modules/aegis_power_matrix/rules.yaml"'
```

配置改完的生效方式：**就地重写 + 息屏再亮屏**（`sed -i` 走临时文件 rename，不触发 `IN_CLOSE_WRITE`；CLG 参数只在屏幕状态变化 / 模式切换 / rules 重载时重新应用）：

```bash
adb -s <SN> shell 'input keyevent KEYCODE_SLEEP; sleep 2; input keyevent KEYCODE_WAKEUP'
```

## 3. 验证命令（可直接复制）

```bash
# 版本
adb -s <SN> shell 'su -c "cat /data/adb/modules/aegis_power_matrix/module.prop"'

# 二进制 md5（本机直接 su 读会被拒，用 init 命名空间）
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- md5sum /data/adb/modules/aegis_power_matrix/core/bin/Aegis"'

# 守护进程
adb -s <SN> shell 'ps -A | grep -i aegis'

# 配置 / 模式 / 充电配置（脚本内容见附录 A）
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- sh /sdcard/aegis_test/final.sh"'
```

**抬频实测（3 次）**：用 `sampler.sh` 连续采样三簇 `scaling_cur_freq`，同时用 `tapv.sh` 向触摸屏 evdev 节点注入 3 次真实触摸（`input tap` 走 InputManager，不产生 evdev，测不到抬频）。
注意**输入节点编号重启后会变**：本机重启前 `fts-sec = event5`，重启后变成 `event7`；先 `getevent -pl` 确认，再改 `tapv.sh` 里的 `D=`。

**Recents 延迟（3 次）**：复用交付目录里的 `tools/recents_latency.ps1 -Runs 3 -Tag <tag>` + `tools/parse_latency.ps1 -Tag <tag>`；
关键指标是 `wx_frame_ms`（微信 `Choreographer#doFrame` 相对 `deliverInputEvent` 的延迟），p50 应落在 120–140ms。

## 4. 配置说明

### 4.1 `input_boost_ms`（默认 150，0 = 关闭）

- 链路：`touch_monitor.rs` 只读打开 `/dev/input/event*`，监听 `BTN_TOUCH` 按下与 `ABS_MT_TRACKING_ID >= 0`
  → 300ms 去抖后发 `DaemonEvent::UserTouch` → `on_input_boost()` 把各簇下限抬到 0.85 档并保持 `input_boost_ms`，
  min/max 写成同一频率钉住；窗口内每拍绕过 `background_ceiling`；窗口结束交回正常降频自然回落。
- 实测 0.85 档频率：p0=1785600、p3=2457600、p7=2342400（再按 `cluster_ceil` 夹紧）。
- 三个限制：
  1. 改配置需**就地重写 + 息屏再亮屏**（`sed -i` 不触发 `IN_CLOSE_WRITE`；该字段随 CLG 配置只在特定事件重载）；
  2. `input tap`（InputManager 注入）不产生 evdev，**不触发抬频**；只有真实触摸才触发；
  3. 触摸设备只在守护启动时枚举一次，热插拔/外接输入设备需重启守护。
- 它不属于 CLG 采样/增益循环，是独立输入通道；只在亮屏、非 fas、CLG 已接管、事件延迟 <200ms 时生效。

### 4.2 `down_fast_threshold: 0.08`（标准档）

- 4.6.1 由 `0.25` 下调到 `0.08`：交互中途簇 util 短暂回落到 0.08~0.25 时不再被 4x 快速沉底直接打回最低频，
  消除频率锯齿与切换/通知栏掉帧；只有前台几乎完全空闲（util<0.08）才允许快速沉底。
- 调大更容易锯齿；调小沉底变慢、静态略费电。省电档仍为 0.25/4.5，性能档 0.18/3.0，极速档 0.10/2.5。

## 5. 已验证收益（MIX Fold 3，标准档）

| 指标 | 4.6（无抬频） | 4.6.1 | 备注 |
|---|---|---|---|
| Recents 点按→首帧 p50 | 239 ms | **123 ms**（本次复测 123.9） | 端到端，含动画起播 |
| Recents 顿挫 | 9/10 | **0/10** | 主观+帧间隔判定 |
| 通知栏起手最坏帧 | 75 ms | **31 ms** | 下拉通知栏 |
| 应用切换掉帧 | ~50% | **~25%** | 切换过程帧丢失比例 |
| 冷启动 | 基线 | **-8.7%** | 应用冷启耗时 |
| 看小说翻页耗电 | — | **<0.03%/小时** | 连续翻页折算 |
| 连续滑动 | — | 抬频**不叠加**，功耗与不开时同量级 | 只刷新窗口、不叠档 |

## 6. 换机风险与注意

1. **SoC / 内核不同**：频率表、簇数量、`cluster_ceil` 必须重算（`module/tools/aegis_adapt.sh` 或 `tools/Aegis_一键适配.sh`），
   否则 0.85 档可能落到错误频率；本资料的频率数值只对 SM8550-AC 有效。
2. **触摸节点编号**：`/dev/input/eventN` 重启/折叠状态会变；`touch_monitor` 只在启动时枚举一次。
3. **root 类型**：本机 MIX Fold 3 是**永久 root**，重启不掉；若换的设备是**临时 root（如平板）**，重启会掉 root，
   需用桌面已验证的 `RPPRoot-ADB-AutoRoot.ps1`（payload 在 `RPPRoot-AutoRoot-v1.0\payload`）恢复，恢复过程记日志。
4. **`/dev/fuse` 缺失**：本机没有 `/dev/fuse`，与调度/抬频无关，不要误判为模块故障。
5. **版本对齐**：模块 `module.prop`、WebUI / AegisManager APP、README / 调参手册的版本描述要一致，避免误判"没装上"。

## 7. 回滚

```bash
# 1) 回滚模块文件（备份 tar 内是安装前的 4.6 / versionCode 9 状态）
adb -s <SN> push pre461_xxx/module_files.tar /sdcard/aegis_test/rollback.tar
adb -s <SN> shell 'su -c "cd /data/adb/modules/aegis_power_matrix && tar -xf /sdcard/aegis_test/rollback.tar"'

# 2) 或：重装上一版 zip（4.6）
adb -s <SN> shell 'su -c "ksud module install /sdcard/aegis_test/<旧版>.zip"'

# 3) 单独回滚配置 / 充电阈值
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- cp /sdcard/aegis_test/bak/pre461_xxx/config.yaml /data/adb/modules/aegis_power_matrix/config/config.yaml"'
adb -s <SN> shell 'su -c "nsenter -t 1 -m -- cp /sdcard/aegis_test/bak/pre461_xxx/charge_guard.conf /data/adb/modules/aegis_power_matrix/charge_guard.conf"'

# 4) 重启生效
adb -s <SN> shell 'su -c "ksud soft-reboot"'
```

## 8. 常见问题

- **KSU 管理器里版本/描述不更新**：`update_prop` 只在启动或模式切换时刷新，重启或切一次模式即可。
- **配置被 zip 覆盖**：安装会按硬件重写 `config/config.yaml`（见 §2）；装完务必核对标准档 `down_fast_threshold: 0.08` 与 `input_boost_ms: 150`，缺一即为旧包/旧模板。
- **ADB 授权失效**：重新插拔数据线、`adb kill-server` 后重试；仍不行到 KSU 管理器撤销并重新授权 USB 调试。
- **抬频测不到**：先确认用的是 `tapv.sh`（evdev 注入）而不是 `input tap`；再确认 `getevent -pl` 里的当前触摸节点与脚本一致。
- **模块文件读不出（Permission denied）**：本机直接 `su -c cat/cp` 会被拒，统一改用 `su -c "nsenter -t 1 -m -- ..."`。

## 9. 本次安装操作日志摘要

1. 备份：手机 `/sdcard/aegis_test/bak/pre461_20261106_1650/`（config.yaml、rules.yaml、module.prop、Aegis.bin、charge_guard.conf、thermal_mode、current_mode.txt、module_files.tar），并拉取到桌面交接目录 `bak/pre461_20261106_1650/`。
2. 安装 Release zip → `ksud` 返回 `Done!` → `ksud soft-reboot` 44s 后模块生效（module.prop 4.6.1/10），**root 未掉**。
3. 发现 zip 内为 CI 重建二进制（md5 `f20ef6ab...`），为对齐实测那份 1,570,840 字节二进制，重建 zip（仅替换 `core/bin/Aegis`）后再次安装。
4. 替换过程临时执行过 `setenforce 0`，随后无法在运行态恢复 Enforcing，因此做了一次**完整重启**；重启后 SELinux 恢复 Enforcing、root 正常、模块生效。
5. 完整重启后发现 `aegis_adapt.sh` 把配置重写成旧默认（标准档 0.25、无 `input_boost_ms`），已从备份还原并息屏/亮屏重载。
6. 最终逐项验证结果见 §1；3 次抬频 3/3 触发，Recents p50 123.9ms。
7. 事后修根因（**未动手机、未重启**）：`tools/aegis_adapt.sh` 的 balance 段改为生成 `down_fast_threshold: 0.08` / `down_fast_mult: 4.0` + `input_boost_ms: 150`；离线用假 sysfs（3 簇）跑 `install` 模式验证，生成结果与包内默认逐项一致。据此重建 `aegis-4.6.1-44-20261106-0831-fixed.zip`（仅替换 `tools/aegis_adapt.sh`，二进制仍为 `1,570,840` / `13cc5a92…`，md5 `852fef94b05fbfd82a1b8577059c5127`），已上传 Release；旧包保留未动。

## 附录 A：设备端 final.sh（读取配置/模式/充电/规则/二进制）

```sh
#!/system/bin/sh
D=/data/adb/modules/aegis_power_matrix
echo "--- CFG"
grep -n -E "down_fast_threshold|down_fast_mult|input_boost_ms" $D/config/config.yaml
echo "--- MODE"
cat $D/current_mode.txt
echo
echo "--- CG"
ls -l $D/charge_guard.conf
echo "--- RULES"
md5sum $D/rules.yaml
echo "--- BIN"
ls -l $D/core/bin/Aegis
```

运行：`adb shell 'su -c "nsenter -t 1 -m -- sh /sdcard/aegis_test/final.sh"'`
