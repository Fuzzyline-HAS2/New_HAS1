#!/usr/bin/env python3
"""Compile actual Telnet/card job engine with deterministic tag/NVS fakes; no USB."""
from pathlib import Path
import os
import subprocess
import tempfile
TESTS = Path(__file__).resolve().parent
DEVICE = TESTS.parent
CASES = ('write213','write215','write216','factory_tlv','standard_write','long_literal','read','held_requires_removal',
         'uid_change_before','uid_change_after','lost_tag','transport_error',
         'uncertain_write','verify_mismatch','disconnect_before','disconnect_after',
         'mode_change_after','cancel_after','timeout_after_write','timeout','timeout_wrap','static_lock',
         'dynamic_lock','protected','readonly','mirror','unknown_model','custom_tlv','custom_lock_tlv',
         'exit_gate','exit_safe_then_unsafe','exit_deferred_recovery','exit_fault_breaks_removal','mode_gate','line_overflow',
         'telnet_parser','settings_persist','invalid_saved','literal_preview',
         'i2c_write','i2c_read','i2c_plus','i2c_2k','i2c_cc','i2c_static_lock','i2c_dynamic_lock',
         'i2c_mirror','i2c_pass_through','i2c_rf_readonly','i2c_read_readonly','i2c_busy','i2c_i2c_locked',
         'i2c_eeprom_error','i2c_session_failure','i2c_changed_session','i2c_late_session_failure',
         'i2c_invalid_session','i2c_invalid_reserved')
def main():
    with tempfile.TemporaryDirectory(prefix='revival-card-upload-tests-') as directory:
        binary=Path(directory)/'card_upload_tests'
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I',str(TESTS/'card_upload_fakes'),'-I',str(TESTS/'fakes'),'-I',str(DEVICE),
                        str(TESTS/'card_upload_tests.cpp'),str(DEVICE/'card_ndef.cpp'),'-o',str(binary)],check=True)
        failed=[case for case in CASES if subprocess.run([str(binary),case]).returncode]
        if failed: raise AssertionError(f'Card upload failures: {failed}')
    print(f'PASS: {len(CASES)} actual card-upload engine cases with ASan/UBSan')
if __name__=='__main__': main()
