"""Shared extraction of production types/constants for host-only sensor tests."""
import re
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]

def write_sensor_types(build: Path) -> None:
    header = (DEVICE / 'HAS1_revival_machine.h').read_text()
    outcome = re.search(r'enum class RfidReadOutcome\s*\{[^}]+\};', header)
    if not outcome:
        raise RuntimeError('Missing production read-outcome enum')
    defines = re.findall(r'^#define RFID_(?:SCAN_BUDGET_MS|RECOVERY_BUDGET_MS|RECOVERY_MAX_ATTEMPTS|DETECT_TIMEOUT_MS|ACTIVATION_RETRIES|REARM_ABSENT_MS)\s+[^\n]+', header, re.M)
    if len(defines) != 6:
        raise RuntimeError('Missing production recovery constants')
    (build / 'rfid_types.inc').write_text(outcome.group() + '\n' + '\n'.join(defines) + '\n')
