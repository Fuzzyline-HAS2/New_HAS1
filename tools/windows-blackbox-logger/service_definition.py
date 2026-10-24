"""Generate the fixed Windows service definition; no credentials enter XML."""
from pathlib import Path
import xml.etree.ElementTree as ET

SERVICE_ID = "HAS1BlackBoxCollector"
WINSW_SHA256 = "b5066b7bbdfba1293e5d15cda3caaea88fbeab35bd5b38c41c913d492aadfc4f"


def service_xml(app_directory, state_directory):
    app, state = Path(app_directory), Path(state_directory)
    root = ET.Element("service")
    def value(name, text):
        ET.SubElement(root, name).text = text
    value("id", SERVICE_ID)
    value("name", "HAS1 Blackbox Collector")
    value("description", "Collects HAS1 device blackbox MQTT logs into local SQLite.")
    value("executable", str(app / ".venv" / "Scripts" / "python.exe"))
    # -E/-s prevent interactive PYTHONPATH/PYTHONHOME/user site-package overrides.
    value("arguments", '-E -s -u "{}" --config "{}" --credential-file "{}"'.format(
        app / "collector.py", state / "config.json", state / "mqtt-password.dpapi"))
    value("workingdirectory", str(app))
    value("startmode", "Automatic")
    value("delayedAutoStart", "true")
    value("hidewindow", "false")
    value("stoptimeout", "30 sec")
    # No stoparguments: WinSW 2.12 ignores stoptimeout in that alternate path.
    # Default stop sends Ctrl+C; Python handles SIGINT, drains, then exits.
    value("stopparentprocessfirst", "false")
    account = ET.SubElement(root, "serviceaccount")
    ET.SubElement(account, "domain").text = "NT AUTHORITY"
    ET.SubElement(account, "user").text = "LocalService"
    for delay in ("10 sec", "30 sec", "60 sec"):
        ET.SubElement(root, "onfailure", action="restart", delay=delay)
    value("resetfailure", "1 hour")
    value("logpath", str(state / "data" / "service-logs"))
    log = ET.SubElement(root, "log", mode="roll-by-size")
    ET.SubElement(log, "sizeThreshold").text = "1024"
    ET.SubElement(log, "keepFiles").text = "4"
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n"


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_directory")
    parser.add_argument("state_directory")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(service_xml(args.app_directory, args.state_directory), encoding="utf-8")
