#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName='List')]
param(
    [Parameter(Mandatory=$true)][string]$Provision,
    [Parameter(Mandatory=$true)][string]$PythonExe,
    [string]$MosquittoDir = (Join-Path $env:ProgramFiles 'mosquitto'),
    [Parameter(Mandatory=$true, ParameterSetName='List')][string]$MacList,
    [Parameter(Mandatory=$true, ParameterSetName='Database')][string]$ServerDatabase,
    [Parameter(ParameterSetName='Database')][ValidateSet('device','training_device')]
    [string[]]$Tables = @('device','training_device'),
    [Parameter(ParameterSetName='Database')][ValidatePattern('^[A-Za-z0-9_-]{1,64}$')]
    [string]$Theme = 'origin',
    [string]$WinSWBinary,
    [switch]$Start
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$BrokerState = Join-Path $env:ProgramData 'HAS1BlackBoxBroker'
$BrokerApp = Join-Path $env:ProgramFiles 'HAS1 BlackBox Broker'
$CollectorState = Join-Path $env:ProgramData 'HAS1BlackBox'
$CollectorApp = Join-Path $env:ProgramFiles 'HAS1 BlackBox'
$Wrapper = Join-Path $BrokerApp 'HAS1BlackBoxBroker.exe'
$BrokerExe = Join-Path $MosquittoDir 'mosquitto.exe'
$PasswdExe = Join-Path $MosquittoDir 'mosquitto_passwd.exe'
$ExpectedHash = 'b5066b7bbdfba1293e5d15cda3caaea88fbeab35bd5b38c41c913d492aadfc4f'
$ProvisionValue = $null

function Check-Exit([string]$Action) {
    if ($LASTEXITCODE -ne 0) { throw "$Action failed (exit $LASTEXITCODE); existing files retained for inspection." }
}
function Assert-SafePath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point not supported: $cursor"
            }
        }
        $next = Split-Path -Parent $cursor
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
}
function Protect-Directory([string]$Path, [string]$ServiceRights = '') {
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    foreach ($sid in @('S-1-5-18','S-1-5-32-544')) {
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
            $identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
    }
    if ($ServiceRights) {
        $identity = New-Object Security.Principal.SecurityIdentifier('S-1-5-19')
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
            $identity,$ServiceRights,'ContainerInherit,ObjectInherit','None','Allow')))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Start-AndWait([string]$Name) {
    Start-Service -Name $Name
    $service = Get-Service -Name $Name
    try { $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(60)) }
    finally { $service.Dispose() }
}

try {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) { throw '64-bit Windows is required.' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run PowerShell as Administrator.' }
    foreach ($path in @($BrokerApp,$BrokerState,$CollectorState,$CollectorApp,$PythonExe,$BrokerExe,$PasswdExe,$Provision)) { Assert-SafePath $path }
    foreach ($path in @($BrokerApp,$BrokerState,$CollectorState,$CollectorApp)) {
        if (Test-Path -LiteralPath $path) { throw "First install only: existing directory preserved: $path" }
    }
    foreach ($name in @('HAS1BlackBoxBroker','HAS1BlackBoxCollector')) {
        if (Get-Service -Name $name -ErrorAction SilentlyContinue) { throw "Service already exists: $name" }
    }
    if (Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object LocalPort -eq 1883) {
        throw 'Port 1883 is already in use. Inspect the existing broker; this script will not stop or replace it.'
    }
    if (-not (Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -eq '172.30.1.43')) {
        throw 'Run on the store-1 PC with IP 172.30.1.43.'
    }
    if (Get-NetFirewallRule -Name 'HAS1BlackBoxMQTT' -ErrorAction SilentlyContinue) { throw 'Existing HAS1BlackBoxMQTT firewall rule retained; inspect it first.' }
    $programRoot = [IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\') + '\'
    foreach ($path in @($PythonExe,$BrokerExe,$PasswdExe)) {
        $resolved = (Resolve-Path -LiteralPath $path).Path
        if (-not $resolved.StartsWith($programRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Install Python and Mosquitto for all users under Program Files.'
        }
    }
    & $PythonExe -E -s -c "import sys,struct; assert sys.version_info >= (3,11); assert struct.calcsize('P')==8"
    Check-Exit 'Python version check'
    # Restrict access before creating the short-lived plaintext password file.
    New-Item -ItemType Directory -Path $BrokerState | Out-Null
    Protect-Directory $BrokerState
    $arguments = @('-E','-s',(Join-Path $PSScriptRoot 'first_install.py'),
        '--provision',$Provision,'--output',$BrokerState,'--passwd-exe',$PasswdExe,'--broker-exe',$BrokerExe)
    if ($PSCmdlet.ParameterSetName -eq 'List') { $arguments += @('--mac-list',$MacList) }
    else { $arguments += @('--server-database',$ServerDatabase,'--theme',$Theme,'--tables') + $Tables }
    & $PythonExe @arguments
    Check-Exit 'Broker preparation'
    $ProvisionValue = Get-Content -LiteralPath $Provision -Raw -Encoding UTF8 | ConvertFrom-Json
    $secret = ConvertTo-SecureString -String $ProvisionValue.collector_password -AsPlainText -Force
    $ProvisionValue = $null
    $install = @{Action='install'; PythonExe=$PythonExe; Config=(Join-Path $BrokerState 'collector-config.json'); MqttPassword=$secret}
    if ($WinSWBinary) { $install.WinSWBinary = $WinSWBinary }
    & (Join-Path $PSScriptRoot 'service.ps1') @install
    Check-Exit 'Collector installation'
    $secret.Dispose()
    $secret = $null
    # Reuse the existing installer-verified WinSW binary; never download a second wrapper.
    $collectorWrapper = Join-Path $env:ProgramFiles 'HAS1 BlackBox\HAS1BlackBoxCollector.exe'
    if ((Get-FileHash -LiteralPath $collectorWrapper -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) { throw 'WinSW hash mismatch.' }
    New-Item -ItemType Directory -Path $BrokerApp | Out-Null
    Protect-Directory $BrokerApp 'ReadAndExecute'
    Copy-Item -LiteralPath $collectorWrapper -Destination $Wrapper
    Copy-Item -LiteralPath (Join-Path $BrokerState 'HAS1BlackBoxBroker.xml') -Destination (Join-Path $BrokerApp 'HAS1BlackBoxBroker.xml')
    Protect-Directory $BrokerState 'ReadAndExecute'
    New-Item -ItemType Directory -Path (Join-Path $BrokerState 'logs') | Out-Null
    Protect-Directory (Join-Path $BrokerState 'logs') 'Modify'
    & $Wrapper install
    Check-Exit 'Broker service registration'
    New-NetFirewallRule -Name 'HAS1BlackBoxMQTT' -DisplayName 'HAS1 Blackbox MQTT (game LAN)' `
        -Direction Inbound -Action Allow -Protocol TCP -LocalPort 1883 -LocalAddress '172.30.1.43' `
        -RemoteAddress LocalSubnet -Profile Private,Domain -Program $BrokerExe | Out-Null
    if ($Start) {
        Start-AndWait 'HAS1BlackBoxBroker'
        Start-AndWait 'HAS1BlackBoxCollector'
        Write-Output 'Both services are running. Confirm a real device batch and ACK in SQLite before calling collection ready.'
    } else { Write-Output 'Installed, not started. Start-Service HAS1BlackBoxBroker; Start-Service HAS1BlackBoxCollector' }
    Write-Output 'Omada, server JSONL and PHP inputs remain disabled until their separate source setup is complete.'
    Write-Output 'Store private-provision.json securely outside the installation folder. Never upload it or commit it.'
} catch {
    Write-Error -ErrorAction Continue $_.Exception.Message
    exit 1
} finally {
    $ProvisionValue = $null
    if (Get-Variable secret -ErrorAction SilentlyContinue) {
        if ($null -ne $secret) { $secret.Dispose() }
    }
}
