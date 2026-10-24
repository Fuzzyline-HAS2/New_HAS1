"""Prepare a NEW broker directory; credentials never enter command arguments.

On Windows call through first-install.ps1, which restricts the directory ACL
before this helper writes any plaintext. No production DB is modified.
"""
import argparse
from contextlib import closing
import ipaddress
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent
TABLES = {"device", "training_device"}


def normalize_mac(value):
    if not isinstance(value, str):
        raise ValueError("MAC must be a string")
    raw = value.strip().replace(":", "").replace("-", "")
    if not re.fullmatch(r"[0-9A-Fa-f]{12}", raw) or raw.upper() in {"000000000000", "FFFFFFFFFFFF"}:
        raise ValueError("Invalid MAC in inventory; use real Wi-Fi STA MACs")
    return raw.upper()


def inventory(mac_list=None, database=None, tables=None):
    if bool(mac_list) == bool(database):
        raise ValueError("Supply exactly one MAC list or server database")
    skipped = 0
    if mac_list:
        lines = Path(mac_list).read_text(encoding="utf-8-sig").splitlines()
        values = [normalize_mac(line) for line in lines if line.strip() and not line.lstrip().startswith("#")]
    else:
        selected = tables or ["device", "training_device"]
        if not selected or any(t not in TABLES for t in selected):
            raise ValueError("Only device and training_device inventory tables are supported")
        path = Path(database).resolve(strict=True)
        values = []
        with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=2)) as db:
            db.execute("PRAGMA query_only=ON")
            db.execute("PRAGMA trusted_schema=OFF")
            for table in selected:
                kind = db.execute("SELECT type FROM sqlite_master WHERE name=?", (table,)).fetchone()
                if kind != ("table",):
                    raise ValueError("Requested inventory table missing or not a table: " + table)
                columns = {row[1] for row in db.execute(f'PRAGMA table_info("{table}")')}
                if not {"mac", "device_type"}.issubset(columns):
                    raise ValueError("Inventory schema missing mac/device_type: " + table)
                rows = db.execute(f'SELECT mac FROM "{table}" WHERE device_type IN (?, ?)',
                                  ("iotglove", "generator"))
                for row in rows:
                    if row[0] in (None, "", "mac not found"):
                        skipped += 1
                    else:
                        values.append(normalize_mac(row[0]))
    result = sorted(set(values))
    if not result or len(result) > 2048:
        raise ValueError("Expected 1..2048 registered device MACs")
    return result, skipped


def read_provision(path):
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if value.get("schema_version") != 1 or value.get("site_id") != "store-1" or value.get("collector_username") != "collector":
        raise ValueError("Unsupported provision identity/schema")
    uri = urlsplit(value.get("broker_uri", ""))
    if uri.scheme != "mqtt" or uri.hostname != "172.30.1.43" or uri.port != 1883 or uri.username or uri.password or uri.path or uri.query or uri.fragment:
        raise ValueError("This store-1 installer requires mqtt://172.30.1.43:1883")
    ipaddress.ip_address(uri.hostname)
    for key in ("device_password", "collector_password"):
        secret = value.get(key)
        maximum = 127 if key == "device_password" else 128
        if not isinstance(secret, str) or not re.fullmatch(r"[A-Za-z0-9_-]{32," + str(maximum) + "}", secret):
            raise ValueError("Provision password length/characters unsupported")
    if value["device_password"] == value["collector_password"]:
        raise ValueError("Collector password must differ from device password")
    return value


def write_new(path, content):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as out:
        out.write(content)


def prepare(output, provision, macs, passwd_exe, *, host="172.30.1.43", port=1883):
    """Caller owns an empty, ACL-protected directory. No overwrite/reuse."""
    output = Path(output).resolve(strict=True)
    if any(output.iterdir()):
        raise ValueError("Broker directory must be empty; existing installation is never overwritten")
    if any(c in str(output) for c in ('\n', '\r', '"', '#')):
        raise ValueError("Unsupported character in broker path")
    # This file is newly created, never an existing hash file (-U hashes again).
    temporary = output / ".passwords-plaintext.tmp"
    try:
        text = "".join(f"{mac}:{provision['device_password']}\n" for mac in macs)
        text += "collector:" + provision["collector_password"] + "\n"
        write_new(temporary, text)
        text = None
        subprocess.run([str(passwd_exe), "-H", "sha512-pbkdf2", "-U", str(temporary)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        hashed = temporary.read_text(encoding="utf-8")
        if len(hashed.splitlines()) != len(macs) + 1 or any(":$7$" not in line for line in hashed.splitlines()):
            raise ValueError("Password tool did not produce expected PBKDF2 hashes")
        temporary.rename(output / "passwords")
    finally:
        temporary.unlink(missing_ok=True)
        # Some tool versions may leave a backup; remove only our own temp name.
        temporary.with_name(temporary.name + ".tmp").unlink(missing_ok=True)
    write_new(output / "acl", (ROOT / "mosquitto" / "acl.example").read_text())
    write_new(output / "mosquitto.conf", f"""listener {port} {host}
allow_anonymous false
password_file {output.as_posix()}/passwords
acl_file {output.as_posix()}/acl
persistence false
max_queued_messages 64
max_inflight_messages 8
message_size_limit 4096
max_packet_size 8192
log_dest stdout
log_type error
log_type warning
log_type notice
""")
    config = json.loads((ROOT / "config.store1.example.json").read_text())
    config.update(host=host, port=port)
    config["telemetry"]["device_import"]["enabled"] = True
    config["telemetry"]["clock_probe"]["enabled"] = True
    write_new(output / "collector-config.json", json.dumps(config, ensure_ascii=False, indent=2) + "\n")
    write_new(output / "registered-macs.txt", "\n".join(macs) + "\n")


def broker_xml(executable, state):
    root = ET.Element("service")
    for key, value in {
        "id": "HAS1BlackBoxBroker", "name": "HAS1 Blackbox MQTT Broker",
        "description": "Isolated authenticated HAS1 diagnostic MQTT broker",
        "executable": str(executable), "arguments": f'-c "{Path(state) / "mosquitto.conf"}"',
        "startmode": "Automatic", "delayedAutoStart": "true", "stoptimeout": "30 sec",
        "hidewindow": "false", "stopparentprocessfirst": "true",
        "logpath": str(Path(state) / "logs"), "resetfailure": "1 hour",
    }.items():
        ET.SubElement(root, key).text = value
    account = ET.SubElement(root, "serviceaccount")
    ET.SubElement(account, "domain").text = "NT AUTHORITY"
    ET.SubElement(account, "user").text = "LocalService"
    for delay in ("10 sec", "30 sec", "60 sec"):
        ET.SubElement(root, "onfailure", action="restart", delay=delay)
    log = ET.SubElement(root, "log", mode="roll-by-size")
    ET.SubElement(log, "sizeThreshold").text = "1024"
    ET.SubElement(log, "keepFiles").text = "4"
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provision", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--passwd-exe", required=True, type=Path)
    parser.add_argument("--broker-exe", required=True, type=Path)
    parser.add_argument("--mac-list", type=Path)
    parser.add_argument("--server-database", type=Path)
    parser.add_argument("--tables", nargs="+", choices=sorted(TABLES))
    args = parser.parse_args()
    try:
        value = read_provision(args.provision)
        macs, skipped = inventory(args.mac_list, args.server_database, args.tables)
        prepare(args.output, value, macs, args.passwd_exe)
        write_new(args.output / "HAS1BlackBoxBroker.xml", broker_xml(args.broker_exe.resolve(strict=True), args.output))
        print(f"Prepared {len(macs)} device accounts; skipped {skipped} unregistered placeholders. No service started.")
    except Exception:
        # Never emit external-tool stdout/stderr or JSON contents with secrets.
        parser.exit(1, "Preparation failed; check inventory/schema, provision format, empty protected output and Mosquitto installation.\n")


if __name__ == "__main__":
    main()
