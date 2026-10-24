"""Optional incident notifications. No credentials, raw responses or URLs in errors.

Called only by the durable outbox worker; never by MQTT ingestion/ACK handling.
HTTP runs in an isolated short-lived child so DNS and slow reads also have a deadline.
"""
from dataclasses import asdict, dataclass
import http.client
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit
import uuid

from service_runtime import read_machine_password

NOTION_VERSION = "2025-09-03"
MAX_BODY = 65536
TOTAL_TIMEOUT = 10.0
NOTION_STATUS_NAMES = {"offline": "In progress", "recovered": "Done", "monitoring_ended": "Not started"}


@dataclass(frozen=True)
class DeliveryResult:
    state: str
    code: str
    retry_after: int = 0
    remote_id: str | None = None


def _uuid(value):
    try:
        return str(uuid.UUID(value))
    except (ValueError, TypeError, AttributeError):
        raise ValueError("Invalid Notion identifier") from None


def validate_config(value, base_path):
    if value is None:
        value = {}
    if not isinstance(value, dict) or set(value) - {"enabled", "credential_file", "slack", "notion"}:
        raise ValueError("Invalid notifications configuration")
    result = {"enabled": value.get("enabled", False)}
    if type(result["enabled"]) is not bool:
        raise ValueError("Invalid notifications enabled flag")
    for channel in ("slack", "notion"):
        entry = value.get(channel, {})
        keys = {"enabled"} | ({"data_source_id"} if channel == "notion" else set())
        if not isinstance(entry, dict) or set(entry) - keys or type(entry.get("enabled", False)) is not bool:
            raise ValueError("Invalid notification channel configuration")
        result[channel] = {"enabled": entry.get("enabled", False)}
        if channel == "notion" and result["enabled"] and entry.get("enabled"):
            result[channel]["data_source_id"] = _uuid(entry.get("data_source_id"))
    if result["enabled"]:
        path = value.get("credential_file")
        if not isinstance(path, str) or not path.strip() or "\0" in path:
            raise ValueError("Notification credential_file is required")
        result["credential_file"] = str((Path(base_path) / path).resolve())
        if not any(result[c]["enabled"] for c in ("slack", "notion")):
            raise ValueError("Enable at least one notification channel")
    return result


def _allowed_url(url, method):
    try:
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.username or parsed.password or
                parsed.port not in (None, 443) or parsed.query or parsed.fragment):
            raise ValueError()
        if parsed.hostname == "hooks.slack.com":
            valid = method == "POST" and re.fullmatch(r"/services/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+/[A-Za-z0-9_-]+", parsed.path)
        elif parsed.hostname == "api.notion.com":
            valid = ((method == "POST" and (parsed.path == "/v1/pages" or
                     re.fullmatch(r"/v1/data_sources/[0-9a-f-]{36}/query", parsed.path))) or
                     (method == "PATCH" and re.fullmatch(r"/v1/pages/[0-9a-f-]{36}", parsed.path)) or
                     (method == "GET" and re.fullmatch(r"/v1/(?:databases|data_sources)/[0-9a-f-]{36}", parsed.path)))
        else:
            valid = False
        if not valid:
            raise ValueError()
        return parsed
    except (ValueError, TypeError):
        raise ValueError("Invalid notification destination") from None


def _http_child(request):
    """Fixed hosts, TLS verification, no redirects or proxy environment use."""
    parsed = _allowed_url(request["url"], request["method"])
    data = None if request["method"] == "GET" else json.dumps(request["body"], ensure_ascii=False).encode("utf-8")
    if data is not None and len(data) > MAX_BODY:
        raise ValueError("Request too large")
    headers = {"Content-Type": "application/json", "User-Agent": "HAS1BlackBox/1"}
    if parsed.hostname == "api.notion.com":
        token = request["token"]
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            raise ValueError("Invalid credential")
        headers.update({"Authorization": "Bearer " + token, "Notion-Version": NOTION_VERSION})
    connection = http.client.HTTPSConnection(parsed.hostname, timeout=TOTAL_TIMEOUT)
    try:
        connection.request(request["method"], parsed.path, data, headers)
        response = connection.getresponse()
        body = response.read(MAX_BODY + 1)
        if len(body) > MAX_BODY:
            return {"status": response.status, "oversized": True}
        return {"status": response.status, "body": body.decode("utf-8", errors="replace"),
                "retry_after": response.getheader("Retry-After", "")[:128]}
    finally:
        connection.close()


def _transport(request, timeout):
    try:
        completed = subprocess.run([sys.executable, "-E", "-s", str(Path(__file__).resolve()), "--http-worker"],
                                   input=json.dumps(request), text=True, capture_output=True,
                                   timeout=max(0.01, timeout), check=False,
                                   creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0)))
        if completed.returncode != 0:
            return {"network_error": True}
        result = json.loads(completed.stdout)
        return result if isinstance(result, dict) else {"network_error": True}
    except (subprocess.TimeoutExpired, OSError, ValueError):
        return {"network_error": True}


def _classify(response, *, safe_retry=False):
    if response.get("network_error"):
        return DeliveryResult("retryable" if safe_retry else "unknown", "network_or_timeout", 30)
    status = response.get("status", 0)
    if status in (429, 529):
        try:
            blocked = json.loads(response.get("body", "{}")).get("additional_data", {}).get("rate_limit_reason") == "public_api_request_blocked"
        except (ValueError, AttributeError):
            blocked = False
        if blocked:
            return DeliveryResult("permanent", "api_access_blocked")
        try:
            delay = max(1, min(86400, int(response.get("retry_after", "30"))))
        except (ValueError, TypeError):
            delay = 30
        return DeliveryResult("retryable", "rate_limited", delay)
    if 200 <= status < 300:
        return None
    if status >= 500 or status == 408:
        return DeliveryResult("retryable" if safe_retry else "unknown", "remote_unavailable", 30)
    return DeliveryResult("permanent", "http_" + str(status))


def notion_database_id(url):
    """Parse a copied Notion DB URL; never connect to the supplied URL itself."""
    try:
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.hostname not in ("notion.so", "www.notion.so", "app.notion.com") or
                parsed.username or parsed.password or parsed.port not in (None, 443)):
            raise ValueError()
        segment = parsed.path.rstrip("/").rsplit("/", 1)[-1]
        match = re.search(r"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}|[0-9a-fA-F]{32})$", segment)
        if not match:
            raise ValueError()
        return _uuid(match.group(1))
    except (ValueError, TypeError):
        raise ValueError("Invalid Notion database URL") from None


def resolve_notion_database(database_url, token, *, transport=_transport):
    """Read-only discovery. Multiple sources require explicit operator selection."""
    try:
        database_id = notion_database_id(database_url)
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            return DeliveryResult("permanent", "invalid_credentials")
        response = transport({"url": "https://api.notion.com/v1/databases/" + database_id,
                              "method": "GET", "body": None, "token": token}, TOTAL_TIMEOUT)
        failure = _classify(response, safe_retry=True)
        if failure:
            return failure
        if response.get("oversized"):
            return DeliveryResult("permanent", "oversized_database_response")
        data = json.loads(response.get("body", ""))
        sources = data.get("data_sources")
        if not isinstance(sources, list) or not sources:
            return DeliveryResult("permanent", "no_data_sources")
        if len(sources) != 1:
            return DeliveryResult("permanent", "multiple_data_sources_choose_explicit_id")
        return DeliveryResult("delivered", "data_source_resolved", remote_id=_uuid(sources[0]["id"]))
    except (ValueError, TypeError, AttributeError, KeyError, OSError, RuntimeError):
        return DeliveryResult("permanent", "invalid_database_or_credentials")


def _notion_schema(response, source_id):
    """Validate only the supported properties; never repair a remote schema."""
    failure = _classify(response, safe_retry=True)
    if failure:
        return None, failure
    if response.get("oversized"):
        return None, DeliveryResult("permanent", "oversized_schema_response")
    try:
        data = json.loads(response.get("body", ""))
        if (not isinstance(data, dict) or data.get("object") != "data_source" or
                _uuid(data.get("id")) != source_id or not isinstance(data.get("properties"), dict)):
            raise ValueError()
        properties = data["properties"]
    except (ValueError, TypeError):
        return None, DeliveryResult("retryable", "invalid_schema_response", 30)
    for name, kind in (("Name", "title"), ("Incident ID", "rich_text"), ("Summary", "rich_text")):
        prop = properties.get(name)
        if not isinstance(prop, dict) or prop.get("type") != kind or not isinstance(prop.get(kind), dict):
            return None, DeliveryResult("permanent", "notion_schema_mismatch")
    prop = properties.get("Status")
    if not isinstance(prop, dict):
        return None, DeliveryResult("permanent", "notion_schema_mismatch")
    kind = prop.get("type")
    if kind == "rich_text" and isinstance(prop.get(kind), dict):
        return kind, None
    if kind != "status" or not isinstance(prop.get("status"), dict):
        return None, DeliveryResult("permanent", "notion_schema_mismatch")
    options = prop["status"].get("options")
    if not isinstance(options, list) or any(not isinstance(option, dict) or not isinstance(option.get("name"), str) for option in options):
        return None, DeliveryResult("retryable", "invalid_schema_response", 30)
    # Validate all transitions during setup as well as delivery. No case folding,
    # option creation or fallback to Done: monitoring ended is not recovery.
    names = [option["name"] for option in options]
    if any(names.count(name) != 1 for name in NOTION_STATUS_NAMES.values()):
        return None, DeliveryResult("permanent", "notion_status_options_mismatch")
    return kind, None


def check_notion_schema(data_source_id, token, *, transport=_transport):
    """Read-only installation preflight, with no credentials in the result."""
    try:
        source_id = _uuid(data_source_id)
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
            return DeliveryResult("permanent", "invalid_credentials")
        response = transport({"url": "https://api.notion.com/v1/data_sources/" + source_id,
                              "method": "GET", "body": None, "token": token}, TOTAL_TIMEOUT)
        kind, failure = _notion_schema(response, source_id)
        return failure or DeliveryResult("delivered", "schema_valid_" + kind)
    except (ValueError, TypeError, AttributeError, KeyError, OSError, RuntimeError):
        return DeliveryResult("permanent", "invalid_schema_or_credentials")


def _text(value, limit=1800):
    # No arbitrary nested payload dumps: external destinations receive only selected fields.
    return str(value or "").replace("\0", "")[:limit]


class NotificationSender:
    def __init__(self, config, *, credential_reader=read_machine_password, transport=_transport):
        self.config = config
        self.credential_reader = credential_reader
        self.transport = transport

    def send(self, channel, payload):
        if channel not in ("slack", "notion") or not self.config.get("enabled") or not self.config.get(channel, {}).get("enabled"):
            return DeliveryResult("permanent", "channel_disabled")
        try:
            credentials = json.loads(self.credential_reader(self.config["credential_file"]))
            if not isinstance(credentials, dict):
                raise ValueError()
            incident_id = payload.get("incident_id")
            if not isinstance(incident_id, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,160}", incident_id):
                return DeliveryResult("permanent", "invalid_incident_id")
            deadline = time.monotonic() + TOTAL_TIMEOUT
            def call(url, method, body, token=None):
                _allowed_url(url, method)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return {"network_error": True}
                return self.transport({"url": url, "method": method, "body": body, "token": token}, remaining)
            summary = "\n".join(f"{key}: {_text(payload.get(key), 300)}" for key in
                                ("incident_id", "site_id", "device_mac", "game_session_id", "status", "occurred_at"))
            summary += "\n" + _text(payload.get("summary"), 800)
            if channel == "slack":
                url = credentials.get("slack_webhook_url")
                response = call(url, "POST", {"text": "HAS1 incident " + incident_id,
                    "blocks": [{"type": "section", "text": {"type": "plain_text", "text": summary[:2900]}}],
                    "unfurl_links": False, "unfurl_media": False})
                failure = _classify(response)
                if failure:
                    return failure
                return DeliveryResult("delivered", "ok") if response.get("body", "").strip() == "ok" else DeliveryResult("unknown", "unexpected_response")
            token = credentials.get("notion_token")
            if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{10,512}", token):
                return DeliveryResult("permanent", "invalid_credentials")
            source_id = _uuid(self.config["notion"]["data_source_id"])
            status = payload.get("status")
            if not isinstance(status, str) or status not in NOTION_STATUS_NAMES:
                return DeliveryResult("permanent", "unsupported_incident_status")
            response = call(f"https://api.notion.com/v1/data_sources/{source_id}", "GET", None, token)
            status_kind, failure = _notion_schema(response, source_id)
            if failure:
                return failure
            response = call(f"https://api.notion.com/v1/data_sources/{source_id}/query", "POST",
                            {"filter": {"property": "Incident ID", "rich_text": {"equals": incident_id}}, "page_size": 2}, token)
            failure = _classify(response, safe_retry=True)
            if failure:
                return failure
            if response.get("oversized"):
                return DeliveryResult("permanent", "oversized_query_response")
            try:
                found = json.loads(response.get("body", ""))
                pages = found["results"]
                if not isinstance(pages, list):
                    raise ValueError()
                if len(pages) > 1 or found.get("has_more"):
                    return DeliveryResult("permanent", "duplicate_incident_records")
                page_id = _uuid(pages[0]["id"]) if pages else None
            except (ValueError, KeyError, TypeError):
                return DeliveryResult("retryable", "invalid_query_response", 30)
            def rich(text):
                return [{"type": "text", "text": {"content": _text(text)}}]
            properties = {"Name": {"title": rich("HAS1 " + incident_id)},
                          "Incident ID": {"rich_text": rich(incident_id)},
                          "Status": ({"status": {"name": NOTION_STATUS_NAMES[status]}} if status_kind == "status"
                                     else {"rich_text": rich(status)}),
                          "Summary": {"rich_text": rich(summary)}}
            body = {"properties": properties}
            if page_id:
                response = call("https://api.notion.com/v1/pages/" + page_id, "PATCH", body, token)
            else:
                body["parent"] = {"type": "data_source_id", "data_source_id": source_id}
                response = call("https://api.notion.com/v1/pages", "POST", body, token)
            failure = _classify(response, safe_retry=bool(page_id))
            if failure:
                return failure
            try:
                remote_id = _uuid(json.loads(response.get("body", ""))["id"])
            except (ValueError, KeyError, TypeError):
                return DeliveryResult("unknown", "invalid_write_response")
            return DeliveryResult("delivered", "ok", remote_id=remote_id)
        except (ValueError, TypeError, KeyError, OSError, RuntimeError):
            # Never include exception text: it can contain a webhook URL or credential.
            return DeliveryResult("permanent", "configuration_or_credentials")


if __name__ == "__main__":
    mode = sys.argv[1:]
    if mode not in (["--http-worker"], ["--resolve-notion-database"], ["--check-notion-schema"]):
        raise SystemExit("Use the collector notification worker or configuration helper")
    try:
        # Windows PowerShell / .NET redirected stdin may prepend a UTF-8 BOM.
        # Decode bytes explicitly so the protocol also ignores the host locale.
        raw = sys.stdin.buffer.read(MAX_BODY + 1)
        if len(raw) > MAX_BODY:
            raise ValueError()
        request = json.loads(raw.decode("utf-8-sig"))
        if mode == ["--http-worker"]:
            result = _http_child(request)
        elif mode == ["--check-notion-schema"]:
            result = asdict(check_notion_schema(request.get("data_source_id"), request.get("token")))
        else:
            result = asdict(resolve_notion_database(request.get("database_url"), request.get("token")))
    except Exception:
        result = {"network_error": True}
    sys.stdout.write(json.dumps(result))
