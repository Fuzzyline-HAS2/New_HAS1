#!/usr/bin/env python3
"""Compile actual PN532 transport with deterministic SPI bytes and a wrapping clock."""
from pathlib import Path
import os
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
DEVICE = TESTS.parent
CASES = (
    'abort', 'abort_budget', 'no_target', 'target', 'target_uid10', 'wake_sam', 'page', 'version', 'config_drains',
    'status_overrun', 'status_tx_empty', 'response_status_fault', 'status_reserved', 'ack_timeout', 'response_timeout',
    'cumulative_budget', 'wrap_budget', 'expired', 'limited_budget',
    'tag_error', 'page_short', 'page_continuation', 'target_short',
    'target_uid_oversize', 'target_count', 'write_failure', 'status_io_failure',
    'ack_invalid', 'nack', 'response_preamble', 'response_lcs', 'response_checksum',
    'response_postamble', 'response_tfi', 'response_command', 'response_error',
    'response_oversize', 'response_extended',
    'upload_read', 'tag_version', 'tag_version_short', 'upload_write',
    'upload_write_error', 'upload_write_extra', 'upload_write_timeout',
    'upload_write_bounds', 'upload_write_no_target',
    'session_ok', 'session_rxmode_preserved', 'session_wrap', 'session_no_target', 'session_null',
    'session_uid_byte_0', 'session_uid_byte_1', 'session_uid_byte_2', 'session_uid_byte_3',
    'session_uid_byte_4', 'session_uid_byte_5', 'session_uid_byte_6', 'session_21x_header_rejected',
    'session_rxmode_invalid', 'session_disable_timeout',
    'session_ack_crc', 'session_ack_nak', 'session_ack_extra', 'session_ack_checksum',
    'session_restore_rx_timeout', 'session_restore_rx_mismatch', 'session_packet2_host_timeout',
    'session_packet2_reply', 'session_packet2_crc', 'session_packet2_extra',
    'session_read_short', 'session_read_error', 'session_read_nonzero_tail',
    'session_sector0_timeout', 'session_header_changed', 'session_total_budget',
    'session_recovery', 'session_recovery_bad_readback',
)

def main():
    with tempfile.TemporaryDirectory(prefix='revival-pn532-wire-tests-') as directory:
        binary = Path(directory) / 'pn532_transport_tests'
        subprocess.run([
            os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(TESTS / 'fakes'), '-I', str(DEVICE),
            str(DEVICE / 'pn532_transport.cpp'), str(TESTS / 'pn532_transport_tests.cpp'),
            '-o', str(binary),
        ], check=True)
        failed = []
        for case in CASES:
            if subprocess.run([str(binary), case]).returncode:
                failed.append(case)
        if failed:
            raise AssertionError(f"Failed transport cases: {failed}")
    print(f'PASS: {len(CASES)} actual transport cases with ASan/UBSan')

if __name__ == '__main__':
    main()
