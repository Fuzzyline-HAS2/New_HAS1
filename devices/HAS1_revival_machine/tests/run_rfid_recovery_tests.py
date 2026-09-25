#!/usr/bin/env python3
"""Exercise actual production sensor lifecycle and absence-latch logic."""
from pathlib import Path
import os
import subprocess
import tempfile
from run_host_tests import extract_functions
from sensor_test_support import write_sensor_types
TESTS = Path(__file__).resolve().parent
DEVICE = TESTS.parent
CASES = ('startup_success', 'startup_report_once', 'unique_near', 'unique_contact', 'unique_far', 'normal_timed_absence',
         'tight_clean_absence', 'rf_off_failure', 'rf_on_failure', 'gain_failure', 'payload_error', 'transport_failure', 'scan_budget',
         'scan_budget_wrap', 'recovery_bounded', 'recovery_wrap', 'recovery_budget',
         'recovery_success', 'approval_blocks_recovery', 'health_deferred_usb', 'unknown_breaks_absence',
         'upload_blank_select', 'upload_absence', 'upload_recovery', 'upload_fault')

def main():
    sensor = (DEVICE / 'sensor.ino').read_text()
    with tempfile.TemporaryDirectory(prefix='revival-rfid-recovery-tests-') as directory:
        build = Path(directory)
        write_sensor_types(build)
        (build / 'low_level.inc').write_text(sensor[sensor.index('static GainMode currentGain'):
              sensor.index('/**\n * @brief RFID(=PN532) 세팅')])
        (build / 'rfid_init.inc').write_text(extract_functions(DEVICE / 'sensor.ino', {'RfidInit'}))
        (build / 'observe.inc').write_text(extract_functions(DEVICE / 'approval.ino',
              {'ObserveGameplayTag', 'ObserveGameplayTagOutcome'}))
        binary = build / 'rfid_recovery_tests'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(build), '-I', str(TESTS / 'fakes'), '-I', str(DEVICE),
                        str(TESTS / 'rfid_recovery_tests.cpp'), '-o', str(binary)], check=True)
        failed = [case for case in CASES if subprocess.run([str(binary), case]).returncode]
        if failed:
            raise AssertionError(f'Failed recovery cases: {failed}')
    print(f'PASS: {len(CASES)} production sensor/recovery cases with ASan/UBSan')

if __name__ == '__main__':
    main()
