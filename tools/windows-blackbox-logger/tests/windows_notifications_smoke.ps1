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
$mockNotionToken = 'ntn_has1_windows_smoke_test_only'
$mockNotionSource = '4030bd38-10bf-8090-aa3f-000b6435ed61'
$helperPath = Join-Path $AppDirectory 'notifications.py'
$originalHelper = [IO.File]::ReadAllBytes($helperPath)
$helperModePath = Join-Path $AppDirectory 'notifications.smoke-mode'
$helperCalledPath = Join-Path $AppDirectory 'notifications.smoke-called'
$helperStagePath = Join-Path $AppDirectory 'notifications.smoke-stage'
if ((Test-Path -LiteralPath $helperModePath) -or (Test-Path -LiteralPath $helperCalledPath) -or (Test-Path -LiteralPath $helperStagePath)) { throw 'Unexpected Notion smoke helper files already exist.' }
function Get-HelperSmokeStage {
    if (-not (Test-Path -LiteralPath $helperStagePath)) { return 'not_started' }
    $stage = [IO.File]::ReadAllText($helperStagePath)
    if ($stage -cnotmatch '^[a-z_0-9\r\n]+$') { return 'invalid_stage_marker' }
    return $stage.Trim().Replace("`n", ',').Replace("`r", '')
}
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

    # Replace only the stopped, disposable installation's CLI helper. This drives
    # the real PowerShell stdin/preflight/DPAPI path without contacting Notion.
    $fakeHelper = @'
import json
from pathlib import Path
import sys
stage_path = Path(__file__).with_suffix(".smoke-stage")
def mark(stage):
    with stage_path.open("a", encoding="utf-8") as stream:
        stream.write(stage + "\n")
mark("started")
assert sys.argv[1:] == ["--check-notion-schema"], "unexpected CLI arguments"
mark("argv_ok")
raw_bytes = sys.stdin.buffer.read(65537)
mark("stdin_bytes_" + str(len(raw_bytes)))
mark("utf8_bom_" + str(int(raw_bytes.startswith(b"\xef\xbb\xbf"))))
assert len(raw_bytes) <= 65536, "oversized preflight stdin"
raw_input = raw_bytes.decode("utf-8-sig")
mark("stdin_chars_" + str(len(raw_input)))
request = json.loads(raw_input)
mark("json_parsed")
assert request == {
    "data_source_id": "4030bd38-10bf-8090-aa3f-000b6435ed61",
    "token": "ntn_has1_windows_smoke_test_only",
}, "unexpected preflight stdin"
mark("stdin_match")
mark("mode_read")
mode = Path(__file__).with_suffix(".smoke-mode").read_text(encoding="utf-8").strip()
mark("mode_loaded")
Path(__file__).with_suffix(".smoke-called").write_text(mode, encoding="utf-8")
if mode == "reject":
    print(json.dumps({"state": "permanent", "code": "notion_schema_mismatch"}))
elif mode == "accept":
    print(json.dumps({"state": "delivered", "code": "schema_valid_status"}))
else:
    raise RuntimeError("Unexpected smoke mode")
'@
    [IO.File]::WriteAllText($helperPath, $fakeHelper, $utf8)
    function global:Read-Host {
        param([string]$Prompt, [switch]$AsSecureString)
        if (-not $AsSecureString) { throw 'Credential prompt must be hidden.' }
        if ($Prompt -eq 'Slack Incoming Webhook URL (hidden)') {
            return ConvertTo-SecureString 'https://hooks.slack.com/services/TEST/TEST/TEST' -AsPlainText -Force
        }
        if ($Prompt -eq 'Notion integration token (hidden)') {
            return ConvertTo-SecureString 'ntn_has1_windows_smoke_test_only' -AsPlainText -Force
        }
        throw 'Unexpected credential prompt.'
    }
    try {
        $beforeNotion = [Convert]::ToBase64String([IO.File]::ReadAllBytes($configPath))
        $credentialsBefore = @((Get-ChildItem -LiteralPath $StateDirectory -Filter 'notifications-*.dpapi').Name | Sort-Object) -join '|'
        [IO.File]::WriteAllText($helperModePath, 'reject', $utf8)
        $failure = $null
        try {
            $setupOutput = & (Join-Path $AppDirectory 'configure-notifications.ps1') -StateDirectory $StateDirectory -EnableSlack -EnableNotion -NotionDataSourceId $mockNotionSource 6>&1
        } catch { $failure = $_.Exception.Message }
        if ($null -eq $failure -or -not $failure.Contains('Previous config is retained.')) { throw 'Rejected Notion schema did not fail safely.' }
        if ($failure.Contains($mockNotionToken) -or $failure.Contains($mockWebhook)) { throw 'Preflight failure exposed a credential.' }
        if (-not (Test-Path -LiteralPath $helperCalledPath) -or [IO.File]::ReadAllText($helperCalledPath) -ne 'reject') { throw ('Direct-ID schema preflight failed; synthetic helper stages: ' + (Get-HelperSmokeStage)) }
        Write-Host ('Notion synthetic preflight stages: ' + (Get-HelperSmokeStage))
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($configPath)) -cne $beforeNotion) { throw 'Rejected Notion schema changed config.' }
        $credentialsAfter = @((Get-ChildItem -LiteralPath $StateDirectory -Filter 'notifications-*.dpapi').Name | Sort-Object) -join '|'
        if ($credentialsAfter -cne $credentialsBefore) { throw 'Rejected Notion schema left an extra DPAPI file.' }

        Remove-Item -LiteralPath $helperCalledPath -Force
        Remove-Item -LiteralPath $helperStagePath -Force -ErrorAction SilentlyContinue
        [IO.File]::WriteAllText($helperModePath, 'accept', $utf8)
        $setupOutput = (& (Join-Path $AppDirectory 'configure-notifications.ps1') -StateDirectory $StateDirectory -EnableSlack -EnableNotion -NotionDataSourceId $mockNotionSource 6>&1 | Out-String)
        if ($setupOutput.Contains($mockNotionToken) -or $setupOutput.Contains($mockWebhook)) { throw 'Notification setup exposed a credential.' }
        if (-not (Test-Path -LiteralPath $helperCalledPath) -or [IO.File]::ReadAllText($helperCalledPath) -ne 'accept') { throw ('Successful direct-ID preflight failed; synthetic helper stages: ' + (Get-HelperSmokeStage)) }
        $configuredRaw = Get-Content -LiteralPath $configPath -Raw
        if ($configuredRaw.Contains($mockNotionToken) -or $configuredRaw.Contains($mockWebhook) -or $configuredRaw.Contains('notion_token')) { throw 'Plaintext credential appeared in Notion config.' }
        $configured = $configuredRaw | ConvertFrom-Json
        if ($configured.host -ne $config.host -or $configured.client_id -ne $config.client_id -or $configured.database -ne $config.database) { throw 'Notion setup modified MQTT settings.' }
        $channels = $configured.telemetry.incidents.notifications
        if (-not $channels.enabled -or -not $channels.slack.enabled -or -not $channels.notion.enabled -or $channels.notion.data_source_id -ne $mockNotionSource) { throw 'Notion channel settings differ.' }
    } finally {
        # Restore before importing notifications in the runtime verification.
        [IO.File]::WriteAllBytes($helperPath, $originalHelper)
        Remove-Item Function:\Read-Host -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $helperModePath, $helperCalledPath, $helperStagePath -Force -ErrorAction SilentlyContinue
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
assert b"hooks.slack.com" not in raw and b"ntn_has1_windows_smoke_test_only" not in raw, "credential blob is not protected"
assert json.loads(read_machine_password(settings["credential_file"])) == {
    "slack_webhook_url": "https://hooks.slack.com/services/TEST/TEST/TEST",
    "notion_token": "ntn_has1_windows_smoke_test_only",
}, "Notion credential roundtrip failed"
print("Notification direct-ID preflight and DPAPI roundtrip PASS")
'@ | Set-Content -LiteralPath $probePath -Encoding UTF8
    & $PythonExe -E -s $probePath $AppDirectory $configPath
    if ($LASTEXITCODE -ne 0) { throw 'Notion DPAPI/config verification failed.' }
    if ((Get-Service HAS1BlackBoxCollector).Status -ne 'Stopped') { throw 'Notification setup unexpectedly started service.' }
    Write-Host 'Notification configuration ACL and config preservation PASS; no network requests sent.'
} finally {
    [IO.File]::WriteAllBytes($helperPath, $originalHelper)
    Remove-Item -LiteralPath $helperModePath, $helperCalledPath, $helperStagePath -Force -ErrorAction SilentlyContinue
    Remove-Item Function:\Read-Host -ErrorAction SilentlyContinue
    [IO.File]::WriteAllBytes($configPath, $originalConfig)
    Remove-Item -LiteralPath $probePath -Force -ErrorAction SilentlyContinue
}
