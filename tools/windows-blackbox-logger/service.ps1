#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0, Mandatory=$true)]
    [ValidateSet('install','update','rollback','start','stop','restart','status','uninstall','credential')]
    [string]$Action,
    [string]$PythonExe,
    [string]$Config,
    [string]$WinSWBinary,
    [Security.SecureString]$MqttPassword
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Started from pwsh 7, Windows PowerShell inherits pwsh's PSModulePath and then
# fails to autoload its own modules (Get-FileHash, Set-Acl). Use the machine default.
$env:PSModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Machine')
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
$ServiceXml = Join-Path $AppDir 'HAS1BlackBoxCollector.xml'
$BackupRoot = Join-Path $AppDir 'backups'
# Everything install copies into AppDir; update/rollback replace exactly this set.
$CodeFiles = @('collector.py','storage.py','protocol.py','service_runtime.py',
    'service_definition.py','requirements.txt','export.py','decode_flash.py',
    'omada.py','telemetry.py','telemetry_runtime.py','timeline.py',
    'evidence_runtime.py','clock_probe.py','gateway.py','import_omada_csv.py',
    'incidents.py','incident_evidence.py','generator_watch.py','notifications.py','retention.py','configure-notifications.ps1','NOTIFICATIONS.md','README.md')

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
function Copy-CodeFiles([string]$From, [string]$To, [string[]]$Files) {
    foreach ($file in $Files) {
        Copy-Item -LiteralPath (Join-Path $From $file) -Destination (Join-Path $To $file) -Force
    }
}
function Test-CodeDirectory([string]$Directory, [bool]$CheckConfig) {
    # Syntax-check every module and run the collector's own config/credential
    # check from this directory, without executing any script entry points.
    $modules = @(Get-ChildItem -LiteralPath $Directory -Filter '*.py' -File | ForEach-Object { $_.FullName })
    Invoke-Checked $ServicePython (@('-E','-s','-c',
        'import py_compile,sys; [py_compile.compile(f, doraise=True) for f in sys.argv[1:]]') + $modules)
    if ($CheckConfig) {
        Invoke-Checked $ServicePython @('-E','-s',(Join-Path $Directory 'collector.py'),'--config',$ConfigTarget,
            '--credential-file',$Credential,'--check-config')
    }
}
function Remove-OldBackups {
    # Keep the ten newest of each kind so pruning never removes the update
    # backups rollback walks through because rollback copies piled up.
    $all = @(Get-ChildItem -LiteralPath $BackupRoot -Directory -ErrorAction SilentlyContinue)
    foreach ($kind in @(
            { $_.Name -notlike '*.restored' -and $_.Name -notlike '*-pre-rollback' },
            { $_.Name -like '*.restored' },
            { $_.Name -like '*-pre-rollback' })) {
        $all | Where-Object $kind | Sort-Object Name -Descending | Select-Object -Skip 10 |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    }
}
function Install-Requirements([string]$Directory) {
    Invoke-Checked $ServicePython @('-E','-s','-m','pip','install','--requirement',(Join-Path $Directory 'requirements.txt'))
}
function Update-ServiceCode([string]$Source, [string[]]$Files, [string]$Label, [string]$BackupSuffix) {
    # Stage and verify before touching the service, back up what is installed,
    # then swap while stopped. Any failure after the stop restores the backup
    # and the previous running state. Config, credential and data are untouched.
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $staging = Join-Path $AppDir ".staging-$stamp"
    $backup = Join-Path $BackupRoot ($stamp + $BackupSuffix)
    if (Test-Path -LiteralPath $backup) { throw "Backup $backup already exists; retry in a second." }
    New-Item -ItemType Directory -Path $staging | Out-Null
    try {
        # Files the source lacks (an older backup) keep their installed copy.
        Copy-CodeFiles $AppDir $staging @($CodeFiles | Where-Object { Test-Path -LiteralPath (Join-Path $AppDir $_) })
        Copy-CodeFiles $Source $staging $Files
        $requirementsChanged = (Get-FileHash -LiteralPath (Join-Path $staging 'requirements.txt')).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $AppDir 'requirements.txt')).Hash
        # New code may import a package only its new requirements add; then the
        # full config check waits until pip has run after the stop.
        Test-CodeDirectory $staging (-not $requirementsChanged)
        $changed = @($CodeFiles | Where-Object {
            $old = Join-Path $AppDir $_
            -not (Test-Path -LiteralPath $old) -or
                (Get-FileHash -LiteralPath $old).Hash -ne (Get-FileHash -LiteralPath (Join-Path $staging $_)).Hash
        })
        if ($changed.Count -eq 0) {
            Write-Output 'Installed code already matches; nothing to do.'
            $script:UpdateResult = 'unchanged'
            return
        }
        Write-Output "Changed files: $($changed -join ', ')"

        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Copy-CodeFiles $AppDir $backup @($CodeFiles | Where-Object { Test-Path -LiteralPath (Join-Path $AppDir $_) })
        Copy-Item -LiteralPath $ServiceXml -Destination $backup -Force
        Set-Content -LiteralPath (Join-Path $backup 'SOURCE.txt') -Encoding UTF8 -Value "$Label from $Source"

        $service = Get-InstalledService
        $wasRunning = $service.Status -ne 'Stopped'
        $service.Dispose()
        try {
            if ($wasRunning) { Invoke-Wrapper 'stop' }
            if ($requirementsChanged) { Install-Requirements $staging }
            Copy-CodeFiles $staging $AppDir $CodeFiles
            Invoke-Checked $ServicePython @('-E','-s',(Join-Path $AppDir 'service_definition.py'),$AppDir,$StateDir,$ServiceXml)
            Set-ProtectedAcl $AppDir $false
            Test-CodeDirectory $AppDir $true
            if ($wasRunning) { Invoke-Wrapper 'start' }
        } catch {
            $failure = $_
            Write-Warning "Update failed ($($failure.Exception.Message)); restoring $backup"
            # Each step on its own so one failure (e.g. pip offline) cannot skip
            # the restart; the service must not be left down by an update.
            $restored = $true
            $steps = [ordered]@{
                'restore code' = { Copy-CodeFiles $backup $AppDir @($CodeFiles | Where-Object { Test-Path -LiteralPath (Join-Path $backup $_) }) }
                'restore service XML' = { Copy-Item -LiteralPath (Join-Path $backup (Split-Path -Leaf $ServiceXml)) -Destination $ServiceXml -Force }
                'reinstall previous requirements' = { if ($requirementsChanged) { Install-Requirements $AppDir } }
                'reapply ACL' = { Set-ProtectedAcl $AppDir $false }
                'restart service' = {
                    $current = Get-InstalledService
                    if ($wasRunning -and $null -ne $current -and $current.Status -ne 'Running') { Invoke-Wrapper 'start' }
                    if ($null -ne $current) { $current.Dispose() }
                }
            }
            foreach ($step in $steps.GetEnumerator()) {
                try { & $step.Value } catch {
                    $restored = $false
                    Write-Warning "Restore step '$($step.Key)' failed: $($_.Exception.Message)"
                }
            }
            if ($restored) {
                # The backup now equals the installed code; keeping it would make
                # the next rollback a silent no-op.
                Remove-Item -LiteralPath $backup -Recurse -Force
                Write-Warning 'Previous code restored and previous service state resumed.'
            } else {
                Write-Warning "Restore incomplete. Copy files from $backup into $AppDir and run service.cmd start."
            }
            throw $failure
        }
        $script:UpdateResult = 'applied'
        Write-Output "Backup of previous code: $backup"
        $state = if ($wasRunning) { 'restarted' } else { 'left stopped' }
        Write-Output "Updated; service $state. Check $StateDir\data\collector.log for the new run."
    } finally {
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
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
            Copy-CodeFiles $PSScriptRoot $AppDir $CodeFiles
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
    } elseif ($Action -in @('update','rollback')) {
        if ($null -eq $service) { throw 'Service is not installed. Use service.cmd install.' }
        $service.Dispose()
        foreach ($required in @($ServicePython, $ServiceXml, $ConfigTarget)) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Installed service is incomplete: $required is missing." }
        }
        if (-not (Test-Path -LiteralPath $Credential -PathType Leaf)) { throw 'Service credential is missing. Use service.cmd credential.' }
        Assert-NoReparse $AppDir
        if ($Action -eq 'update') {
            $source = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
            if ($source -eq [IO.Path]::GetFullPath($AppDir).TrimEnd('\')) { throw 'Run update from a source checkout, not the installed folder.' }
            Assert-NoReparse $source
            $missing = @($CodeFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $source $_) -PathType Leaf) })
            if ($missing.Count) { throw "Source is missing: $($missing -join ', ')" }
            Update-ServiceCode $source $CodeFiles 'update' ''
            Remove-OldBackups
        } else {
            # Newest backup made by update; skip ones already restored and the
            # copies rollback itself makes, so repeated rollback walks backwards.
            $latest = Get-ChildItem -LiteralPath $BackupRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -notlike '*.restored' -and $_.Name -notlike '*-pre-rollback' } |
                Sort-Object Name -Descending | Select-Object -First 1
            if ($null -eq $latest) { throw "No backup to roll back to in $BackupRoot." }
            $files = @($CodeFiles | Where-Object { Test-Path -LiteralPath (Join-Path $latest.FullName $_) -PathType Leaf })
            if ($files -notcontains 'collector.py') { throw "Backup $($latest.FullName) has no collector.py." }
            Write-Warning 'Rollback restores code only. If the newer code migrated a database schema, restore the matching data backup too (see README).'
            $script:UpdateResult = $null
            Update-ServiceCode $latest.FullName $files 'rollback' '-pre-rollback'
            # Marked either way so the next rollback goes one step further back.
            Rename-Item -LiteralPath $latest.FullName -NewName ($latest.Name + '.restored')
            if ($script:UpdateResult -eq 'unchanged') {
                Write-Output "Backup $($latest.Name) already matches the installed code; nothing was rolled back. Run rollback again to go further back."
            } else {
                Write-Output "Restored from $($latest.FullName)"
            }
            Remove-OldBackups
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
