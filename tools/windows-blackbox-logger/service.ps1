#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0, Mandatory=$true)]
    [ValidateSet('install','start','stop','restart','status','uninstall','credential')]
    [string]$Action,
    [string]$PythonExe,
    [string]$Config,
    [string]$WinSWBinary,
    [Security.SecureString]$MqttPassword
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Windows PowerShell 5.1 can evaluate parameter defaults before PSScriptRoot is
# populated under -File. Resolve the default only after script binding finishes.
if (-not $PSBoundParameters.ContainsKey('Config')) {
    $Config = Join-Path $PSScriptRoot 'config.json'
}
$ServiceId = 'HAS1BlackBoxCollector'
$AppDir = Join-Path $env:ProgramFiles 'HAS1 BlackBox'
$StateDir = Join-Path $env:ProgramData 'HAS1BlackBox'
$Wrapper = Join-Path $AppDir 'HAS1BlackBoxCollector.exe'
$ServicePython = Join-Path $AppDir '.venv\Scripts\python.exe'
$Credential = Join-Path $StateDir 'mqtt-password.dpapi'
$ConfigTarget = Join-Path $StateDir 'config.json'
$ExpectedHash = 'b5066b7bbdfba1293e5d15cda3caaea88fbeab35bd5b38c41c913d492aadfc4f'
$DownloadUri = 'https://github.com/winsw/winsw/releases/download/v2.12.0/WinSW.NET461.exe'

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed (exit $LASTEXITCODE)." }
}
function Assert-NoReparse([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not supported: $cursor"
            }
        }
        $parent = Split-Path -Parent $cursor
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    if (Test-Path -LiteralPath $full -PathType Container) {
        $reparse = Get-ChildItem -LiteralPath $full -Force -Recurse |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
            Select-Object -First 1
        if ($reparse) { throw "Reparse points are not supported: $($reparse.FullName)" }
    }
}
function Set-ProtectedAcl([string]$Path, [bool]$Writable) {
    # Numeric SIDs work on localized Windows. Replacing (not adding to) the ACL
    # prevents inherited Everyone/Users Modify from exposing the DPAPI blob/code.
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    foreach ($sid in @('S-1-5-18','S-1-5-32-544','S-1-5-19')) {
        $rights = if ($sid -eq 'S-1-5-19') {
            if ($Writable) { 'Modify' } else { 'ReadAndExecute' }
        } else { 'FullControl' }
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $identity, $rights, 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
    # Reset child ACLs so a preserved state directory cannot retain weaker grants.
    if (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1) {
        Invoke-Checked 'icacls.exe' @((Join-Path $Path '*'), '/reset', '/T', '/Q')
    }
}
function Assert-Hash([string]$Path) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
        throw 'WinSW SHA256 mismatch; refusing to execute the wrapper.'
    }
}
function Save-Credential([Security.SecureString]$Secret) {
    if ($null -eq $Secret -or $Secret.Length -eq 0) { throw 'MQTT password must not be empty.' }
    Add-Type -AssemblyName System.Security
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Secret)
    $bytes = $null
    try {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
        if ($plain.Contains([char]0)) { throw 'MQTT password cannot contain NUL.' }
        $bytes = [Text.Encoding]::UTF8.GetBytes($plain)
        if ($bytes.Length -gt 4096) { throw 'MQTT password is too long.' }
        $encrypted = [Security.Cryptography.ProtectedData]::Protect(
            $bytes, $null, [Security.Cryptography.DataProtectionScope]::LocalMachine)
        $temporary = Join-Path $StateDir 'mqtt-password.dpapi.new'
        [IO.File]::WriteAllBytes($temporary, $encrypted)
        Move-Item -LiteralPath $temporary -Destination $Credential -Force
    } finally {
        $plain = $null
        if ($null -ne $bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
    }
}
function Get-InstalledService {
    return Get-Service -Name $ServiceId -ErrorAction SilentlyContinue
}
function Invoke-Wrapper([string]$Command) {
    Assert-NoReparse $AppDir
    Assert-Hash $Wrapper
    Invoke-Checked $Wrapper @($Command)
    # WinSW 2.12 `stop` returns after sending the SCM command, not after the
    # child exits. Wait explicitly before reporting success/removing the entry.
    if ($Command -in @('stop','start','restart')) {
        $waiter = Get-Service -Name $ServiceId
        try {
            $desired = if ($Command -eq 'stop') { 'Stopped' } else { 'Running' }
            $waiter.WaitForStatus([ServiceProcess.ServiceControllerStatus]$desired, [TimeSpan]::FromSeconds(90))
        } finally { $waiter.Dispose() }
    }
}

try {
    if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'The service installer requires 64-bit Windows.'
    }
    if ($Action -eq 'status') {
        $service = Get-InstalledService
        if ($null -eq $service) { Write-Output 'HAS1BlackBoxCollector: not installed'; exit 0 }
        $service | Format-Table Name, Status, StartType -AutoSize
        Write-Output "Database/logs: $StateDir\data"
        exit 0
    }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Open PowerShell or Command Prompt as Administrator before changing the service.'
    }
    $service = Get-InstalledService
    if ($Action -eq 'install') {
        $netRelease = Get-ItemPropertyValue -Path 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -Name Release
        if ($netRelease -lt 528040) { throw '.NET Framework 4.8 or newer is required (supported Windows 10/11).' }
        if ($null -ne $service) { throw 'Service already installed. Use start, stop, restart, status, or uninstall.' }
        if (-not $PythonExe) { throw 'Supply -PythonExe pointing to an all-users Python 3.11+ under Program Files.' }
        $PythonExe = (Resolve-Path -LiteralPath $PythonExe).Path
        $programRoot = [IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\') + '\'
        if (-not $PythonExe.StartsWith($programRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Python must be installed for all users under Program Files; per-user/WindowsApps Python cannot serve LocalService.'
        }
        Assert-NoReparse (Split-Path -Parent $PythonExe)
        Assert-NoReparse $AppDir
        Assert-NoReparse $StateDir
        Invoke-Checked $PythonExe @('-E','-s','-c',
            "import sys,struct; assert sys.version_info >= (3,11), 'Python 3.11+ required'; assert struct.calcsize('P')==8, '64-bit Python required'")
        # Validate before creating or overwriting service files. Reinstallation
        # reuses the retained service configuration; it does not migrate a live DB.
        $existingConfig = Test-Path -LiteralPath $ConfigTarget
        $configInput = if ($existingConfig) { $ConfigTarget } else { (Resolve-Path -LiteralPath $Config).Path }
        $loadedJson = & $PythonExe '-E' '-s' '-c' `
            'import sys,json; sys.path.insert(0,sys.argv[1]); from collector import load_config; print(json.dumps(load_config(sys.argv[2])))' `
            $PSScriptRoot $configInput
        if ($LASTEXITCODE -ne 0) { throw 'Invalid collector configuration.' }
        $loaded = $loadedJson | ConvertFrom-Json
        $database = Join-Path $StateDir 'data\blackbox.sqlite3'
        if ($existingConfig -and $loaded.database -ne $database) {
            throw 'Retained service configuration must use the service data/blackbox.sqlite3 path.'
        }
        if ($existingConfig -and $loaded.PSObject.Properties.Name -contains 'telemetry' -and $null -ne $loaded.telemetry) {
            if ($loaded.telemetry.database -ne (Join-Path $StateDir 'data\telemetry.sqlite3')) {
                throw 'Retained telemetry configuration must use service data/telemetry.sqlite3.'
            }
            if ($loaded.telemetry.PSObject.Properties.Name -contains 'incidents' -and
                $loaded.telemetry.incidents.database -ne (Join-Path $StateDir 'data\incidents.sqlite3')) {
                throw 'Retained incident configuration must use service data/incidents.sqlite3.'
            }
        }
        $ca = $null
        if ($loaded.PSObject.Properties.Name -contains 'tls_ca' -and $loaded.tls_ca) {
            $ca = (Resolve-Path -LiteralPath $loaded.tls_ca).Path
            if (-not (Test-Path -LiteralPath $ca -PathType Leaf)) { throw 'tls_ca is not a file.' }
        }
        if (-not (Test-Path -LiteralPath $Credential) -and $null -eq $MqttPassword) {
            $MqttPassword = Read-Host 'MQTT collector password (hidden)' -AsSecureString
        }
        if ($null -ne $MqttPassword -and $MqttPassword.Length -eq 0) { throw 'MQTT password must not be empty.' }
        $download = $null
        try {
            if ($WinSWBinary) {
                $candidate = (Resolve-Path -LiteralPath $WinSWBinary).Path
            } else {
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                $download = [IO.Path]::GetTempFileName()
                Invoke-WebRequest -UseBasicParsing -Uri $DownloadUri -OutFile $download
                $candidate = $download
            }
            Assert-Hash $candidate
            New-Item -ItemType Directory -Path $AppDir -Force | Out-Null
            New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
            Set-ProtectedAcl $AppDir $false
            Set-ProtectedAcl $StateDir $false
            New-Item -ItemType Directory -Path (Join-Path $StateDir 'data') -Force | Out-Null
            Set-ProtectedAcl (Join-Path $StateDir 'data') $true
            foreach ($file in @('collector.py','storage.py','protocol.py','service_runtime.py',
                'service_definition.py','requirements.txt','export.py','decode_flash.py',
                'omada.py','telemetry.py','telemetry_runtime.py','timeline.py',
                'evidence_runtime.py','clock_probe.py','gateway.py','import_omada_csv.py',
                'incidents.py','notifications.py','configure-notifications.ps1','NOTIFICATIONS.md','README.md')) {
                Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $AppDir $file) -Force
            }
            Copy-Item -LiteralPath $candidate -Destination $Wrapper -Force
            Assert-Hash $Wrapper
            Invoke-Checked $PythonExe @('-E','-s','-m','venv','--clear',(Join-Path $AppDir '.venv'))
            Invoke-Checked $ServicePython @('-E','-s','-m','pip','install','--requirement',(Join-Path $AppDir 'requirements.txt'))
            if (-not $existingConfig) {
                $loaded.database = $database
                if ($loaded.PSObject.Properties.Name -contains 'telemetry' -and $null -ne $loaded.telemetry) {
                    $loaded.telemetry.database = Join-Path $StateDir 'data\telemetry.sqlite3'
                    if ($loaded.telemetry.PSObject.Properties.Name -contains 'incidents') {
                        $loaded.telemetry.incidents | Add-Member -NotePropertyName database -NotePropertyValue (Join-Path $StateDir 'data\incidents.sqlite3') -Force
                    }
                }
                if ($ca) {
                    Copy-Item -LiteralPath $ca -Destination (Join-Path $StateDir 'broker-ca.pem') -Force
                    $loaded.tls_ca = Join-Path $StateDir 'broker-ca.pem'
                }
                [IO.File]::WriteAllText($ConfigTarget, ($loaded | ConvertTo-Json -Depth 12), (New-Object Text.UTF8Encoding($false)))
            }
            if ($null -ne $MqttPassword) { Save-Credential $MqttPassword }
            Invoke-Checked $ServicePython @('-E','-s',(Join-Path $AppDir 'collector.py'),'--config',$ConfigTarget,'--credential-file',$Credential,'--check-config')
            Invoke-Checked $ServicePython @('-E','-s',(Join-Path $AppDir 'service_definition.py'),$AppDir,$StateDir,
                (Join-Path $AppDir 'HAS1BlackBoxCollector.xml'))
            # Re-apply inheritance to everything created by pip/venv before SCM
            # can execute it. Only state/data is writable to LocalService.
            Set-ProtectedAcl $AppDir $false
            Set-ProtectedAcl $StateDir $false
            Set-ProtectedAcl (Join-Path $StateDir 'data') $true
            try {
                Invoke-Wrapper 'install'
            } catch {
                $installFailure = $_
                if ($null -ne (Get-InstalledService)) {
                    try { Invoke-Wrapper 'uninstall' } catch {
                        Write-Warning 'Registration rollback failed. Inspect service.cmd status; data/settings were retained.'
                    }
                }
                throw $installFailure
            }
            Write-Output 'Installed (not started). Run service.cmd start; use service.cmd status to inspect.'
            Write-Output "Configuration: $ConfigTarget"
            Write-Output "Database: $database (existing console data was not copied)"
        } finally {
            if ($download -and (Test-Path -LiteralPath $download)) { Remove-Item -LiteralPath $download -Force }
        }
    } elseif ($Action -eq 'credential') {
        if (-not (Test-Path -LiteralPath $ConfigTarget)) { throw 'Install the service first.' }
        if ($null -ne $service -and $service.Status -ne 'Stopped') { throw 'Stop the service before updating its password.' }
        Assert-NoReparse $StateDir
        if ($null -eq $MqttPassword) { $MqttPassword = Read-Host 'New MQTT collector password (hidden)' -AsSecureString }
        Save-Credential $MqttPassword
        Write-Output 'Credential updated. Run service.cmd start.'
    } else {
        if ($null -eq $service) { throw 'Service is not installed.' }
        if ($Action -eq 'uninstall') {
            if ($service.Status -ne 'Stopped') { Invoke-Wrapper 'stop' }
            $service.Dispose()
            $service = $null
            Invoke-Wrapper 'uninstall'
            Write-Output "Service unregistered. Program files, settings, credentials and logs preserved: $AppDir ; $StateDir"
        } else {
            if ($Action -in @('start','restart')) {
                if (-not (Test-Path -LiteralPath $Credential -PathType Leaf)) { throw 'Service credential is missing. Use service.cmd credential.' }
                Invoke-Checked $ServicePython @('-E','-s',(Join-Path $AppDir 'collector.py'),'--config',$ConfigTarget,'--credential-file',$Credential,'--check-config')
            }
            Invoke-Wrapper $Action
        }
    }
    exit 0
} catch {
    Write-Error -ErrorAction Continue $_.Exception.Message
    exit 1
}
