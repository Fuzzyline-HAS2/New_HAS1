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
# Credentials are never accepted on command lines or printed. A new credential
# file is written before the atomic config switch so prior config stays usable.
$private = @{}
if ($EnableSlack) {
    $private.slack_webhook_url = Read-PrivateText 'Slack Incoming Webhook URL (hidden)'
    if ($private.slack_webhook_url -cnotmatch '^https://hooks\.slack\.com/services/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+$') { throw 'Invalid Slack webhook format.' }
}
if ($EnableNotion) {
    $private.notion_token = Read-PrivateText 'Notion integration token (hidden)'
    if ($private.notion_token -cnotmatch '^[A-Za-z0-9_-]{10,512}$') { throw 'Invalid Notion token format.' }
    if ($NotionDatabaseUrl) {
        $python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
        if (-not (Test-Path -LiteralPath $python -PathType Leaf)) { throw 'Run from the installed collector folder; Python venv is missing.' }
        $lookup = @{ database_url = $NotionDatabaseUrl; token = $private.notion_token } | ConvertTo-Json -Compress
        $result = ($lookup | & $python -E -s (Join-Path $PSScriptRoot 'notifications.py') --resolve-notion-database) | ConvertFrom-Json
        $lookup = $null
        if ($LASTEXITCODE -ne 0 -or $result.state -ne 'delivered') {
            if ($result.code -eq 'multiple_data_sources_choose_explicit_id') { throw 'This database has several data sources. Select the correct source and use -NotionDataSourceId.' }
            throw 'Cannot resolve the Notion data source. Check DB URL, token access and network, then retry.'
        }
        $NotionDataSourceId = $result.remote_id
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
try {
    [IO.File]::WriteAllBytes($credentialPath, $encrypted)
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($sid)), 'FullControl', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $rule = New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier('S-1-5-19')), 'Read', 'Allow')
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $credentialPath -AclObject $acl
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
    [IO.File]::WriteAllText($configTemporary, ($config | ConvertTo-Json -Depth 30), $utf8)
    # Preserve installed config ACL (LocalService read, administrators write).
    Set-Acl -LiteralPath $configTemporary -AclObject (Get-Acl -LiteralPath $configPath)
    [IO.File]::Replace($configTemporary, $configPath, $null)
    $configCommitted = $true
    Write-Host 'Notification settings saved. Restart HAS1BlackBoxCollector to load them. No test message was sent.'
    Write-Host 'Previously created notifications-*.dpapi files can be removed after checking the active credential_file.'
} catch {
    if (-not $configCommitted) { Remove-Item -LiteralPath $credentialPath -Force -ErrorAction SilentlyContinue }
    throw 'Notification setup failed. Previous config is retained; check incident setup and Windows permissions.'
} finally {
    Remove-Item -LiteralPath $configTemporary -Force -ErrorAction SilentlyContinue
}
