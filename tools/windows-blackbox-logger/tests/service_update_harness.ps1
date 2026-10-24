# Exercises service.ps1's update/restore functions against temporary folders.
# SCM, WinSW, ACL and pip calls are replaced by recorders; nothing is installed.
#requires -Version 5.1
param([Parameter(Mandatory=$true)][string]$PythonExe)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Same as service.ps1: CI starts this from pwsh 7, whose PSModulePath hides Get-FileHash.
$env:PSModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Machine')
$root = Split-Path -Parent $PSScriptRoot
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'service.ps1'), [ref]$null, [ref]$null)
$ast.FindAll({ $args[0] -is [Management.Automation.Language.FunctionDefinitionAst] }, $false) |
    ForEach-Object { . ([scriptblock]::Create($_.Extent.Text)) }
$CodeFiles = ([scriptblock]::Create(($ast.FindAll({ $args[0] -is [Management.Automation.Language.AssignmentStatementAst] -and
    $args[0].Left.Extent.Text -eq '$CodeFiles' }, $false) | Select-Object -First 1).Right.Extent.Text)).Invoke()

$script:Calls = New-Object Collections.Generic.List[string]
$script:Fail = @{}
$script:Status = 'Running'
function Invoke-Wrapper([string]$Command) {
    $script:Calls.Add($Command)
    if ($script:Fail[$Command]) { $script:Fail[$Command] = $false; throw "simulated $Command failure" }
    $script:Status = if ($Command -eq 'stop') { 'Stopped' } else { 'Running' }
}
function Get-InstalledService { [pscustomobject]@{ Status = $script:Status } | Add-Member ScriptMethod Dispose {} -PassThru }
function Set-ProtectedAcl([string]$Path, [bool]$Writable) { }
function Install-Requirements([string]$Directory) {
    $script:Calls.Add('pip')
    if ($script:Fail['pip']) { throw 'simulated pip failure' }
}
function Test-CodeDirectory([string]$Directory, [bool]$CheckConfig) {
    $script:Calls.Add("check:$CheckConfig")
    $modules = @(Get-ChildItem -LiteralPath $Directory -Filter '*.py' -File | ForEach-Object { $_.FullName })
    Invoke-Checked $ServicePython (@('-E','-s','-c','import py_compile,sys; [py_compile.compile(f, doraise=True) for f in sys.argv[1:]]') + $modules)
}

$temp = Join-Path $env:TEMP ('HAS1UpdateHarness-' + [guid]::NewGuid().ToString('N'))
$results = [ordered]@{}
function New-Case([string]$Name) {
    $script:AppDir = Join-Path $temp "$Name\app"
    $script:Source = Join-Path $temp "$Name\src"
    $script:StateDir = Join-Path $temp "$Name\state"
    $script:BackupRoot = Join-Path $AppDir 'backups'
    $script:ServiceXml = Join-Path $AppDir 'HAS1BlackBoxCollector.xml'
    $script:ServicePython = $PythonExe
    foreach ($dir in @($AppDir, $Source, $StateDir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    foreach ($file in $CodeFiles) {
        Set-Content -LiteralPath (Join-Path $AppDir $file) -Value 'old = 1' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $Source $file) -Value 'old = 1' -Encoding UTF8
    }
    # service_definition.py is executed after the swap to write the XML.
    $definition = 'import sys; open(sys.argv[3], "w").write("<service/>")'
    Set-Content -LiteralPath (Join-Path $AppDir 'service_definition.py') -Value $definition -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Source 'service_definition.py') -Value $definition -Encoding UTF8
    Set-Content -LiteralPath $ServiceXml -Value '<old/>' -Encoding UTF8
    $script:Calls.Clear(); $script:Fail = @{}; $script:Status = 'Running'; $script:UpdateResult = $null
}
function Read-App([string]$File) { (Get-Content -LiteralPath (Join-Path $AppDir $File) -Raw).Trim() }
function Get-Backups { @(Get-ChildItem -LiteralPath $BackupRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) }

try {
    New-Case 'unchanged'
    Update-ServiceCode $Source $CodeFiles 'update' '' | Out-Null
    $results.unchanged = @{ result = $UpdateResult; calls = @($Calls); backups = Get-Backups }

    New-Case 'applied'
    Set-Content -LiteralPath (Join-Path $Source 'omada.py') -Value 'new = 2' -Encoding UTF8
    Update-ServiceCode $Source $CodeFiles 'update' '' | Out-Null
    $results.applied = @{ result = $UpdateResult; calls = @($Calls); omada = Read-App 'omada.py'
        xml = (Get-Content -LiteralPath $ServiceXml -Raw).Trim(); backups = Get-Backups; status = $Status }

    New-Case 'start_fails'
    Set-Content -LiteralPath (Join-Path $Source 'omada.py') -Value 'new = 2' -Encoding UTF8
    $Fail['start'] = $true
    $err = $null
    try { Update-ServiceCode $Source $CodeFiles 'update' '' 3>$null | Out-Null } catch { $err = $_.Exception.Message }
    $results.start_fails = @{ error = $err; calls = @($Calls); omada = Read-App 'omada.py'
        xml = (Get-Content -LiteralPath $ServiceXml -Raw).Trim(); backups = Get-Backups; status = $Status }

    New-Case 'pip_fails'
    Set-Content -LiteralPath (Join-Path $Source 'requirements.txt') -Value 'paho-mqtt==9.9' -Encoding UTF8
    $Fail['pip'] = $true
    $err = $null
    try { Update-ServiceCode $Source $CodeFiles 'update' '' 3>$null | Out-Null } catch { $err = $_.Exception.Message }
    $results.pip_fails = @{ error = $err; calls = @($Calls); requirements = Read-App 'requirements.txt'
        backups = Get-Backups; status = $Status }

    New-Case 'stopped_stays_stopped'
    $script:Status = 'Stopped'
    Set-Content -LiteralPath (Join-Path $Source 'omada.py') -Value 'new = 2' -Encoding UTF8
    Update-ServiceCode $Source $CodeFiles 'update' '' | Out-Null
    $results.stopped_stays_stopped = @{ calls = @($Calls); status = $Status }

    New-Case 'prune'
    New-Item -ItemType Directory -Path $BackupRoot | Out-Null
    foreach ($i in 10..25) {
        foreach ($suffix in @('', '-pre-rollback', '.restored')) {
            New-Item -ItemType Directory -Path (Join-Path $BackupRoot ("202601$i-000000" + $suffix)) | Out-Null
        }
    }
    Remove-OldBackups
    $left = Get-Backups
    $results.prune = @{
        plain = @($left | Where-Object { $_ -notlike '*-pre-rollback' -and $_ -notlike '*.restored' }).Count
        pre = @($left | Where-Object { $_ -like '*-pre-rollback' }).Count
        restored = @($left | Where-Object { $_ -like '*.restored' }).Count
        oldest_plain = ($left | Where-Object { $_ -notlike '*-*-*' -and $_ -notlike '*.restored' } | Sort-Object | Select-Object -First 1) }
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
$results | ConvertTo-Json -Depth 5 -Compress
