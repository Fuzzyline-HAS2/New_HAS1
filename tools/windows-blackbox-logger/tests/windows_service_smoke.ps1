# Deliberately NOT discovered by unittest. Only run in a disposable elevated
# Windows CI runner: it really registers and exercises the Windows SCM service.
#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PythonExe)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$serviceScript = Join-Path $root 'service.ps1'
$id = 'HAS1BlackBoxCollector'
$app = Join-Path $env:ProgramFiles 'HAS1 BlackBox'
$state = Join-Path $env:ProgramData 'HAS1BlackBox'
$fixture = Join-Path $env:ProgramFiles ('HAS1BlackBoxSmokePython-' + [guid]::NewGuid().ToString('N'))
$temp = Join-Path $env:TEMP ('HAS1BlackBoxSmoke-' + [guid]::NewGuid().ToString('N'))
$database = Join-Path $state 'data\blackbox.sqlite3'
$log = Join-Path $state 'data\collector.log'
if ((Get-Service -Name $id -ErrorAction SilentlyContinue) -or
    (Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath $state)) {
    throw 'Smoke test requires a disposable runner without an existing HAS1 installation.'
}
function Invoke-Service([string[]]$Arguments) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $serviceScript @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Service command failed: $($Arguments -join ' ')" }
}
function Wait-Until([scriptblock]$Condition, [string]$Description, [int]$Seconds=40) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out: $Description"
}
function Get-CollectorProcess {
    $scm = Get-CimInstance Win32_Service -Filter "Name='$id'"
    if ($scm.ProcessId -eq 0) { return $null }
    $currentParent = $scm.ProcessId
    $leaf = $null
    for ($depth = 0; $depth -lt 5; $depth++) {
        $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$currentParent" |
            Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -like '*collector.py*' } |
            Select-Object -First 1
        if ($null -eq $child) { break }
        $leaf = $child
        $currentParent = $child.ProcessId
    }
    # Windows venv python.exe may be a redirector with the actual interpreter as
    # its child. Crash the leaf so the redirector exits too, not an orphaned peer.
    return $leaf
}
try {
    New-Item -ItemType Directory -Path $temp | Out-Null
    # setup-python lives in the runner cache. Copy its standalone base into a
    # protected all-users path to exercise the real production precondition.
    $base = & $PythonExe -c 'import sys; print(sys.base_prefix)'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate Python base.' }
    Copy-Item -LiteralPath $base -Destination $fixture -Recurse
    & icacls.exe $fixture '/inheritance:r' '/grant:r' '*S-1-5-18:(OI)(CI)F' `
        '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)RX' '/T' '/Q'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot protect fixture Python.' }
    $python = Join-Path $fixture 'python.exe'
    $config = Join-Path $temp 'config.json'
    Copy-Item -LiteralPath (Join-Path $root 'config.example.json') -Destination $config
    # Deliberately unreachable local broker: Windows startup must not depend on
    # broker availability; collector retry logic runs without fake-success ACKs.
    $settings = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
    $settings.port = 1
    $settings.client_id = 'has1-service-smoke'
    [IO.File]::WriteAllText($config, ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    $secret = ConvertTo-SecureString 'ci-only-local-password' -AsPlainText -Force
    # Invoke in this process to pass SecureString, never a real password in argv.
    & $serviceScript install -PythonExe $python -Config $config -MqttPassword $secret
    if ($LASTEXITCODE -ne 0) { throw 'Install failed.' }
    if ((Get-Service $id).Status -ne 'Stopped') { throw 'Install unexpectedly started collector.' }
    $scm = Get-CimInstance Win32_Service -Filter "Name='$id'"
    if ($scm.StartName -ne 'NT AUTHORITY\LocalService') { throw 'Unexpected service identity.' }
    $reg = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$id"
    if ($reg.Start -ne 2 -or $reg.DelayedAutoStart -ne 1) { throw 'Delayed auto-start missing.' }
    Invoke-Service @('start')
    Wait-Until { (Test-Path -LiteralPath $database) -and (Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Collector running' -Quiet) } 'LocalService collector readiness'
    & $python -c 'import sqlite3,sys; c=sqlite3.connect("file:"+sys.argv[1]+"?mode=ro",uri=True); assert c.execute("PRAGMA user_version").fetchone()[0] == 1' $database
    if ($LASTEXITCODE -ne 0) { throw 'LocalService SQLite initialization failed.' }
    # Cross-session mutex: a console with a DIFFERENT DB and same broker/client
    # identity must fail before connecting or creating a database.
    $consoleConfig = Join-Path $temp 'console.json'
    $settings.database = Join-Path $temp 'console.sqlite3'
    [IO.File]::WriteAllText($consoleConfig, ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    & $python -c 'import subprocess,sys; r=subprocess.run(sys.argv[1:],timeout=10); assert r.returncode==1, "duplicate collector must fail"' `
        (Join-Path $app '.venv\Scripts\python.exe') (Join-Path $app 'collector.py') --config $consoleConfig `
        --credential-file (Join-Path $state 'mqtt-password.dpapi')
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $settings.database)) {
        throw 'Cross-session duplicate MQTT collector was not refused.'
    }
    $oldProcess = Get-CollectorProcess
    if ($null -eq $oldProcess) { throw 'Collector child not found.' }
    Stop-Process -Id $oldProcess.ProcessId -Force
    Wait-Until {
        $new = Get-CollectorProcess
        $null -ne $new -and $new.ProcessId -ne $oldProcess.ProcessId
    } 'SCM recovery after collector crash' 60
    Wait-Until { @((Select-String -LiteralPath $log -Pattern 'Collector running')).Count -ge 2 } 'Recovered collector readiness'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Service @('stop')
    if ($watch.Elapsed.TotalSeconds -gt 35) { throw 'Service stop exceeded bounded timeout.' }
    if (-not (Select-String -LiteralPath $log -Pattern 'stopped cleanly after draining' -Quiet)) {
        throw 'Stop used forced termination instead of graceful Python shutdown.'
    }
    Start-Sleep -Seconds 12
    if ((Get-Service $id).Status -ne 'Stopped') { throw 'Manual stop unexpectedly triggered recovery.' }
    $before = (Get-FileHash -LiteralPath $database -Algorithm SHA256).Hash
    Invoke-Service @('uninstall')
    if (Get-Service $id -ErrorAction SilentlyContinue) { throw 'SCM entry remains.' }
    if ((Get-FileHash -LiteralPath $database -Algorithm SHA256).Hash -ne $before) { throw 'Uninstall modified database.' }
    foreach ($file in @('config.json','mqtt-password.dpapi','data\collector.log')) {
        if (-not (Test-Path -LiteralPath (Join-Path $state $file))) { throw "Uninstall removed $file" }
    }
    # Re-registration uses retained configuration/credential; no prompt and no
    # accidental migration/reset of the existing service database.
    Invoke-Service @('install','-PythonExe',$python,'-Config',$config)
    Invoke-Service @('start')
    Wait-Until { @((Select-String -LiteralPath $log -Pattern 'Collector running')).Count -ge 3 } 'Reinstalled collector readiness'
    Invoke-Service @('stop')
    Invoke-Service @('uninstall')
    Write-Output 'PASS: LocalService, DPAPI, delayed start, duplicate guard, crash recovery, graceful stop, reinstall, preservation.'
} finally {
    if (Get-Service $id -ErrorAction SilentlyContinue) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $serviceScript uninstall
    }
    # Preserve logs in CI workspace before deleting the disposable fixture.
    if (Test-Path -LiteralPath (Join-Path $state 'data')) {
        Copy-Item -LiteralPath (Join-Path $state 'data') -Destination (Join-Path $root 'service-smoke-results') -Recurse -Force
    }
    foreach ($directory in @($app,$state,$fixture,$temp)) {
        if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    }
}
