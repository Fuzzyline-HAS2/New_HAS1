#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$StateDirectory = (Join-Path $env:ProgramData 'HAS1BlackBox'),
    [switch]$EnableSlack,
    [switch]$EnableNotion,
    [string]$NotionDataSourceId = '',
    [string]$NotionDatabaseUrl = ''
)
$ErrorActionPreference = 'Stop'
if (-not $EnableSlack -and -not $EnableNotion) { throw 'Choose -EnableSlack and/or -EnableNotion.' }
$StateDirectory = [IO.Path]::GetFullPath($StateDirectory)
$configPath = Join-Path $StateDirectory 'config.json'
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { throw 'Install the collector first; config.json is missing.' }
$cursor = Get-Item -LiteralPath $StateDirectory
while ($null -ne $cursor) {
    if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse-point state directories are not supported.' }
    $cursor = $cursor.Parent
}
if ((Get-Item -LiteralPath $configPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse-point config is not supported.' }
if ($EnableNotion -and [bool]$NotionDataSourceId -eq [bool]$NotionDatabaseUrl) { throw 'Supply exactly one of -NotionDataSourceId or -NotionDatabaseUrl.' }
if ($EnableNotion -and $NotionDataSourceId) {
    $parsedId = [Guid]::Empty
    if (-not [Guid]::TryParse($NotionDataSourceId, [ref]$parsedId)) { throw 'Supply the Notion data source UUID, not the database URL.' }
    $NotionDataSourceId = $parsedId.ToString()
}
function Read-PrivateText([string]$Prompt) {
    $secure = Read-Host $Prompt -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr); $secure.Dispose() }
}
function Invoke-NotionHelper([string]$PythonPath, [ValidateSet('--resolve-notion-database', '--check-notion-schema')][string]$Mode, [hashtable]$Request) {
    # Explicit pipes avoid Windows PowerShell native-pipeline stdin forwarding.
    # Only the fixed mode and installed script path appear on the command line.
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $PythonPath
    $start.Arguments = '-E -s -X utf8 "' + (Join-Path $PSScriptRoot 'notifications.py') + '" ' + $Mode
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $payload = [Text.Encoding]::UTF8.GetBytes(($Request | ConvertTo-Json -Compress))
    try {
        if ($payload.Length -gt 65536 -or -not $process.Start()) { throw 'Notion helper could not start.' }
        # Drain both output streams while writing input to prevent pipe deadlocks.
        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        $writeTask = $process.StandardInput.BaseStream.WriteAsync($payload, 0, $payload.Length)
        if (-not $writeTask.Wait(30000)) { throw 'Notion helper input timed out.' }
        # Close the byte stream directly; the unused text writer must not emit
        # an encoding preamble after the JSON on older .NET/PowerShell versions.
        $process.StandardInput.BaseStream.Close()
        if (-not $process.WaitForExit(30000) -or -not $outputTask.Wait(5000) -or -not $errorTask.Wait(5000)) { throw 'Notion helper timed out.' }
        if ($process.ExitCode -ne 0) { throw 'Notion helper failed.' }
        return ($outputTask.Result | ConvertFrom-Json)
    } catch {
        # Never forward helper stderr or exception text: either may contain input.
        throw 'Cannot run Notion validation. Check the installed Python environment and network. Previous config is retained.'
    } finally {
        [Array]::Clear($payload, 0, $payload.Length)
        try { if (-not $process.HasExited) { $process.Kill() } } catch { }
        $process.Dispose()
    }
}
# Credentials are never accepted on command lines or printed. A new credential
# file is written before the atomic config switch so prior config stays usable.
$private = @{}
if ($EnableSlack) {
    $private.slack_webhook_url = Read-PrivateText 'Slack Incoming Webhook URL (hidden)'
    if ($private.slack_webhook_url -cnotmatch '^https://hooks\.slack\.com/services/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+$') { throw 'Invalid Slack webhook format.' }
}
if ($EnableNotion) {
    $python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw 'Run from the installed collector folder; Python venv is missing.' }
    $private.notion_token = Read-PrivateText 'Notion integration token (hidden)'
    if ($private.notion_token -cnotmatch '^[A-Za-z0-9_-]{10,512}$') { throw 'Invalid Notion token format.' }
    if ($NotionDatabaseUrl) {
        $result = Invoke-NotionHelper $python '--resolve-notion-database' @{ database_url = $NotionDatabaseUrl; token = $private.notion_token }
        if ($result.state -ne 'delivered') {
            if ($result.code -eq 'multiple_data_sources_choose_explicit_id') { throw 'This database has several data sources. Select the correct source and use -NotionDataSourceId.' }
            throw 'Cannot resolve the Notion data source. Check DB URL, token access and network, then retry.'
        }
        $NotionDataSourceId = $result.remote_id
    }
    # Read-only preflight for either input form; never save unusable credentials/config.
    try {
        $result = Invoke-NotionHelper $python '--check-notion-schema' @{ data_source_id = $NotionDataSourceId; token = $private.notion_token }
        if ($result.state -ne 'delivered') {
            throw 'Notion preflight failed.'
        }
    } catch {
        throw 'Cannot validate Notion schema. Check token access, network and property types/options in NOTIFICATIONS.md. Previous config is retained.'
    }
}
Add-Type -AssemblyName System.Security
$bytes = [Text.Encoding]::UTF8.GetBytes(($private | ConvertTo-Json -Compress))
try { $encrypted = [Security.Cryptography.ProtectedData]::Protect($bytes, $null, [Security.Cryptography.DataProtectionScope]::LocalMachine) }
finally { [Array]::Clear($bytes, 0, $bytes.Length); $private.Clear() }
$credentialName = 'notifications-' + [Guid]::NewGuid().ToString('N') + '.dpapi'
$credentialPath = Join-Path $StateDirectory $credentialName
$configTemporary = Join-Path $StateDirectory ('config-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$configCommitted = $false
$setupStage = 'credential_write'
try {
    [IO.File]::WriteAllBytes($credentialPath, $encrypted)
    $setupStage = 'credential_acl'
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($sid)), 'FullControl', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $rule = New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier('S-1-5-19')), 'Read', 'Allow')
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $credentialPath -AclObject $acl
    $setupStage = 'config_read'
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ($null -eq $config.telemetry) { throw 'telemetry configuration is required.' }
    if ($null -eq $config.telemetry.incidents -or -not $config.telemetry.incidents.enabled) { throw 'Configure and verify incident collection before enabling notifications.' }
    $settings = [ordered]@{
        enabled = $true
        credential_file = $credentialName
        slack = @{ enabled = [bool]$EnableSlack }
        notion = @{ enabled = [bool]$EnableNotion }
    }
    if ($EnableNotion) { $settings.notion.data_source_id = $NotionDataSourceId }
    $config.telemetry.incidents | Add-Member -NotePropertyName notifications -NotePropertyValue $settings -Force
    $utf8 = New-Object Text.UTF8Encoding($false)
    $setupStage = 'config_write'
    [IO.File]::WriteAllText($configTemporary, ($config | ConvertTo-Json -Depth 30), $utf8)
    # Preserve installed config ACL (LocalService read, administrators write).
    $setupStage = 'config_acl'
    Set-Acl -LiteralPath $configTemporary -AclObject (Get-Acl -LiteralPath $configPath)
    $setupStage = 'config_replace'
    # PowerShell coerces $null to an empty string for this string parameter;
    # File.Replace rejects an empty backup path. NullString passes a CLR null.
    [IO.File]::Replace($configTemporary, $configPath, [NullString]::Value)
    $configCommitted = $true
    Write-Host 'Notification settings saved. Restart HAS1BlackBoxCollector to load them. No test message was sent.'
    Write-Host 'Previously created notifications-*.dpapi files can be removed after checking the active credential_file.'
} catch {
    if (-not $configCommitted) { Remove-Item -LiteralPath $credentialPath -Force -ErrorAction SilentlyContinue }
    # Only fixed stage identifiers are reported; exception text may contain secrets.
    $retention = if ($configCommitted) { 'New config is saved.' } else { 'Previous config is retained.' }
    throw "Notification setup failed [stage=$setupStage]. $retention Check incident setup and Windows permissions."
} finally {
    Remove-Item -LiteralPath $configTemporary -Force -ErrorAction SilentlyContinue
}
