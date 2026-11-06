# deploy_input_boost.ps1 - one-click deploy: replace Aegis daemon binary + config, hot restart daemon
# Does NOT reinstall the KSU module and does NOT reboot the phone.
# Backups kept on device: /sdcard/aegis_test/bak/Aegis.before_boost, /sdcard/aegis_test/config.before_boost.yaml
# Roll back with rollback_input_boost.ps1
param(
  [string]$Serial = "85920f66",
  [string]$AdbPath = "C:\Users\Administrator\AppData\Local\Android\Sdk\platform-tools\adb.exe"
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$bin = Join-Path $here "Aegis.input-boost.aarch64"
$cfg = Join-Path $here "config.balance.input-boost.yaml"
$mod = "/data/adb/modules/aegis_power_matrix"
$dst = "$mod/core/bin/Aegis"

Write-Output "[1/4] pushing artifacts"
& $AdbPath -s $Serial push $bin /data/local/tmp/Aegis.boost | Out-Null
& $AdbPath -s $Serial push $cfg /data/local/tmp/config.boost.yaml | Out-Null

Write-Output "[2/4] backup current binary + config"
$bk = "mkdir -p /sdcard/aegis_test/bak; " +
      "cp $dst /sdcard/aegis_test/bak/Aegis.before_boost; " +
      "cp $mod/config/config.yaml /sdcard/aegis_test/config.before_boost.yaml; " +
      "md5sum /sdcard/aegis_test/bak/Aegis.before_boost"
& $AdbPath -s $Serial shell "su -c '$bk'"

Write-Output "[3/4] install config + binary, kill stale daemon first (avoid Text file busy)"
$inst = "cp /data/local/tmp/config.boost.yaml $mod/config/config.yaml; " +
        "chmod 644 $mod/config/config.yaml; " +
        "killall -9 Aegis 2>/dev/null; sleep 1; " +
        "cp /data/local/tmp/Aegis.boost $dst; chmod 755 $dst; " +
        "nohup $dst > /dev/null 2>&1 &"
& $AdbPath -s $Serial shell "su -c '$inst'"
Start-Sleep -Seconds 4

Write-Output "[4/4] verify"
& $AdbPath -s $Serial shell "su -c 'echo pid=`$(pidof Aegis); md5sum $dst; grep -n input_boost_ms $mod/config/config.yaml; tail -4 $mod/logs/daemon.log'"
