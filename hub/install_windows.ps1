# Установка моста раций на Windows 10/11 (запускать в PowerShell от администратора).
#   powershell -ExecutionPolicy Bypass -File install_windows.ps1
# Что делает: правила брандмауэра (UDP 47000, TCP 47080), задача Планировщика «WalkieHub»
# (при включении компьютера, без окна, перезапуск при сбое), запуск.
# Папка установки — та, где лежит этот файл (walkie_hub.py, wt_proto.py, hub.json).
$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path "$dir\hub.json")) { throw "нет $dir\hub.json — скопируйте hub.example.json и впишите ключ сети" }

# pythonw.exe — Python без чёрного окна
$py = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $py) {
  $py3 = (& py -3 -c "import sys;print(sys.executable)" 2>$null)
  if ($py3) { $py = Join-Path (Split-Path $py3) "pythonw.exe" }
}
if (-not $py -or -not (Test-Path $py)) { throw "не найден Python 3 (pythonw.exe)" }
Write-Host "Python: $py"

foreach ($r in @(@{n="Мост раций UDP 47000"; p="UDP"; port=47000}, @{n="Мост раций TCP 47080"; p="TCP"; port=47080})) {
  Get-NetFirewallRule -DisplayName $r.n -ErrorAction SilentlyContinue | Remove-NetFirewallRule
  New-NetFirewallRule -DisplayName $r.n -Direction Inbound -Protocol $r.p -LocalPort $r.port -Action Allow -Profile Any | Out-Null
}

$action = New-ScheduledTaskAction -Execute $py -Argument "`"$dir\walkie_hub.py`" --config `"$dir\hub.json`"" -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
  -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
Unregister-ScheduledTask -TaskName "WalkieHub" -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName "WalkieHub" -Action $action -Trigger $trigger -Settings $settings -Principal $principal | Out-Null
Start-ScheduledTask -TaskName "WalkieHub"
Start-Sleep -Seconds 3
$p = Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" | Where-Object { $_.CommandLine -like "*walkie_hub.py*" }
if ($p) { Write-Host "Мост запущен (процесс $($p.ProcessId)). Страница: http://localhost:47080/" }
else { Write-Host "Мост не виден среди процессов — смотрите $dir\hub.log" }
