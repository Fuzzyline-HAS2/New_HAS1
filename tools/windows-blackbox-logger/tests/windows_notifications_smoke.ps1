# Disposable Windows CI only. The caller has installed a STOPPED fixture service.
# Exercise the actual hidden-prompt setup, DPAPI and ACL without any network calls.
#requires -Version 5.1
#requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppDirectory,
    [Parameter(Mandatory=$true)][string]$StateDirectory,
    [Parameter(Mandatory=$true)][string]$PythonExe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$configPath = Join-Path $StateDirectory 'config.json'
$originalConfig = [IO.File]::ReadAllBytes($configPath)
$probePath = Join-Path $StateDirectory ('notifications-probe-' + [Guid]::NewGuid().ToString('N') + '.py')
$utf8 = New-Object Text.UTF8Encoding($false)
$mockWebhook = 'https://hooks.slack.com/services/TEST/TEST/TEST'
$readHostBefore = Get-Item Function:\Read-Host -ErrorAction SilentlyContinue
if ($null -ne $readHostBefore) { throw 'Unexpected Read-Host override in smoke environment.' }
try {
    if ((Get-Service HAS1BlackBoxCollector).Status -ne 'Stopped') { throw 'Notification smoke requires the fixture service stopped.' }
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $config.telemetry | Add-Member -NotePropertyName incidents -NotePropertyValue ([pscustomobject]@{
        enabled = $true
        database = (Join-Path $StateDirectory 'data\incidents.sqlite3')
        notifications = [pscustomobject]@{ enabled = $false }
    }) -Force
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 30), $utf8)
    function global:Read-Host {
        param([string]$Prompt, [switch]$AsSecureString)
        if (-not $AsSecureString) { throw 'Credential prompt must be hidden.' }
        return ConvertTo-SecureString 'https://hooks.slack.com/services/TEST/TEST/TEST' -AsPlainText -Force
    }
    & (Join-Path $AppDirectory 'configure-notifications.ps1') -StateDirectory $StateDirectory -EnableSlack
    Remove-Item Function:\Read-Host
    $configuredRaw = Get-Content -LiteralPath $configPath -Raw
    if ($configuredRaw.Contains($mockWebhook) -or $configuredRaw.Contains('slack_webhook_url')) { throw 'Plaintext credential appeared in config.' }
    $configured = $configuredRaw | ConvertFrom-Json
    if ($configured.host -ne $config.host -or $configured.client_id -ne $config.client_id -or $configured.database -ne $config.database) { throw 'Notification setup modified MQTT settings.' }
    if (-not $configured.telemetry.incidents.notifications.enabled -or -not $configured.telemetry.incidents.notifications.slack.enabled -or $configured.telemetry.incidents.notifications.notion.enabled) { throw 'Notification channel settings differ.' }
    $credentialPath = Join-Path $StateDirectory $configured.telemetry.incidents.notifications.credential_file
    $acl = Get-Acl -LiteralPath $credentialPath
    if (-not $acl.AreAccessRulesProtected) { throw 'Notification credential inherits broad permissions.' }
    $rules = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]))
    if ($rules.Count -ne 3) { throw 'Unexpected notification credential ACL count.' }
    foreach ($rule in $rules) {
        $sid = $rule.IdentityReference.Value
        if ($sid -notin @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-19') -or $rule.AccessControlType -ne 'Allow' -or $rule.IsInherited) { throw 'Unexpected notification credential ACL principal.' }
        if ($sid -eq 'S-1-5-19') {
            if (($rule.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write) -ne 0 -or ($rule.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Read) -eq 0) { throw 'LocalService credential permission is not read-only.' }
        } elseif (($rule.FileSystemRights -band [Security.AccessControl.FileSystemRights]::FullControl) -ne [Security.AccessControl.FileSystemRights]::FullControl) { throw 'Administrator credential access missing.' }
    }
    @'
import json
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
from service_runtime import read_machine_password
from notifications import validate_config
config = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
settings = validate_config(config["telemetry"]["incidents"]["notifications"], Path(sys.argv[2]).parent)
raw = Path(settings["credential_file"]).read_bytes()
assert b"hooks.slack.com" not in raw, "credential blob is not protected"
decoded = json.loads(read_machine_password(settings["credential_file"]))
assert decoded == {"slack_webhook_url": "https://hooks.slack.com/services/TEST/TEST/TEST"}, "credential roundtrip failed"
print("Notification configuration DPAPI and schema PASS")
'@ | Set-Content -LiteralPath $probePath -Encoding UTF8
    & $PythonExe -E -s $probePath $AppDirectory $configPath
    if ($LASTEXITCODE -ne 0) { throw 'Notification DPAPI/config verification failed.' }
    if ((Get-Service HAS1BlackBoxCollector).Status -ne 'Stopped') { throw 'Notification setup unexpectedly started service.' }
    Write-Host 'Notification configuration ACL and config preservation PASS; no network requests sent.'
} finally {
    Remove-Item Function:\Read-Host -ErrorAction SilentlyContinue
    [IO.File]::WriteAllBytes($configPath, $originalConfig)
    Remove-Item -LiteralPath $probePath -Force -ErrorAction SilentlyContinue
}
