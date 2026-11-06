# rollback_input_boost.ps1 - one-click rollback to the pre-boost daemon binary + config
# Hot restart only (kill + cp + nohup). No module reinstall, no phone reboot.
param(
  [string]$Serial = "85920f66",
  [string]$AdbPath = "C:\Users\Administrator\AppData\Local\Android\Sdk\platform-tools\adb.exe"
)
$ErrorActionPreference = "Stop"
$mod = "/data/adb/modules/aegis_power_matrix"
$dst = "$mod/core/bin/Aegis"
$bak = "/sdcard/aegis_test/bak/Aegis.before_boost"
$cfgbak = "/sdcard/aegis_test/config.before_boost.yaml"

$has = (& $AdbPath -s $Serial shell "su -c 'ls $bak $cfgbak'") -join ""
if ($has -notmatch 'Aegis.before_boost') { Write-Output "MISSING_BACKUP: $bak"; exit 1 }

$rb = "cp $cfgbak $mod/config/config.yaml; chmod 644 $mod/config/config.yaml; " +
      "killall -9 Aegis 2>/dev/null; sleep 1; " +
      "cp $bak $dst; chmod 755 $dst; " +
      "nohup $dst > /dev/null 2>&1 &"
& $AdbPath -s $Serial shell "su -c '$rb'"
Start-Sleep -Seconds 4
& $AdbPath -s $Serial shell "su -c 'echo pid=`$(pidof Aegis); md5sum $dst; grep -c input_boost_ms $mod/config/config.yaml'"
