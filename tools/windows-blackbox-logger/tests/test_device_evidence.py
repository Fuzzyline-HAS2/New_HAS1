"""Device event schema compatibility through durable storage and both exports."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from helpers import MAC, RING, TOPIC, payload, record
from protocol import decode_record, named_values, parse_batch
from storage import Store
from export import export
from timeline import align_event, device_event


# Signed wire ID 0x80000000 is a valid causal ID, not the unavailable sentinel.
HIGH_ID = -2147483648
MAX_ID = -1
GLOVE_CASES = (
    ('button.edge', 8, (HIGH_ID, 1, 2, MAX_ID),
     ('press_id', 'down', 'disposition', 'command_sequence'),
     (2147483648, 1, 2, 4294967295), ('id', 'boolean', 'enum', 'uint32')),
    ('button.context', 8, (HIGH_ID, 0x03060305, 2, MAX_ID),
     ('press_id', 'context_bits', 'queued_events', 'in_flight_sequence'),
     (2147483648, 0x03060305, 2, 4294967295), ('id', 'bits', 'count', 'uint32')),
    ('button.suppressed', 8, (9, 10000, 4, 0),
     ('press_count', 'window_ms', 'trace_limit', 'reserved'),
     (9, 10000, 4, 0), ('count', 'ms', 'count', 'raw')),
    ('button.command', 10, (HIGH_ID, MAX_ID, 3, 1),
     ('press_id', 'command_sequence', 'stage', 'result'),
     (2147483648, 4294967295, 3, 1), ('id', 'uint32', 'enum', 'enum')),
    ('motor.applied', 8, (1, 2, HIGH_ID, 0),
     ('motor', 'source', 'press_id', 'reserved'),
     (1, 2, 2147483648, 0), ('boolean', 'enum', 'id', 'raw')),
    ('motor.suppressed', 8, (4, 0, 1000, 16),
     ('edge_count', 'final_motor', 'window_ms', 'edge_limit'),
     (4, 0, 1000, 16), ('count', 'boolean', 'ms', 'count')),
)


def fields(raw):
    return named_values(decode_record(raw))['fields']


class DeviceEvidenceTests(unittest.TestCase):
    def test_glove_field_contract_keeps_high_bit_ids_and_raw_signed_values(self):
        for label, kind, values, names, expected, units in GLOVE_CASES:
            with self.subTest(label=label):
                decoded = fields(record(kind=kind, text=label.encode(), values=values))
                self.assertEqual(tuple(decoded), names)
                for name, value, unit, wire in zip(names, expected, units, values):
                    self.assertEqual(decoded[name], {'value': value, 'unit': unit, 'raw_signed': wire})

    def test_recovery_stages_preserve_elapsed_history_and_stage_specific_detail(self):
        for stage, detail in ((1, 30000), (1, 35000), (1, 185000), (1, 1805000),
                              (1, 210000),  # Historical threshold remains literal.
                              (2, 1 | 2 | 4 | 8), (3, 0), (4, 0), (5, 0), (99, -1)):
            with self.subTest(stage=stage):
                raw = record(kind=7, text=b'wifi.offline.recovery', values=(stage, -2, 65535, detail))
                decoded = fields(raw)
                self.assertEqual(decoded['stage'], {'value': stage, 'unit': 'enum', 'raw_signed': stage})
                self.assertEqual(decoded['outage_ms']['value'], 4294967294)
                self.assertEqual(decoded['outage_ms']['raw_signed'], -2)
                self.assertEqual(decoded['attempts']['value'], 65535)
                self.assertEqual(decoded['detail']['value'], detail & 0xffffffff)
                self.assertEqual(decoded['detail']['raw_signed'], detail)

    def test_glove_restart_backoff_preserves_saturated_unsigned_history(self):
        cases = (
            ('restart.request', (2, -1, 0, 0), ('cause', 'attempts', 'reserved_0', 'reserved_1'), (2, 4294967295, 0, 0)),
            ('restart.request', (3, HIGH_ID, 0, 0), ('cause', 'attempts', 'reserved_0', 'reserved_1'), (3, 2147483648, 0, 0)),
            ('restart.wait', (3, -1, 1805000, 1775000), ('cause', 'attempts', 'total_wait_ms', 'remaining_ms'), (3, 4294967295, 1805000, 1775000)),
            ('restart.wait_cancel', (3, -1, 40000, 0), ('cause', 'attempts', 'elapsed_ms', 'reserved'), (3, 4294967295, 40000, 0)),
            ('restart.backoff_reset', (-1, 600000, -2147483648, 0), ('previous_attempts', 'healthy_ms', 'worker_sequence', 'reserved'), (4294967295, 600000, 2147483648, 0)),
        )
        for label, values, names, expected in cases:
            with self.subTest(label=label):
                decoded = fields(record(kind=7, text=label.encode(), values=values))
                self.assertEqual(tuple(decoded), names)
                self.assertEqual(tuple(decoded[name]['value'] for name in names), expected)
                self.assertEqual(tuple(decoded[name]['raw_signed'] for name in names), values)

    def test_button_command_unknown_results_and_motor_off_are_not_reinterpreted(self):
        for stage, result in ((0, -1), (3, 0), (3, 1), (5, 0), (5, 1), (5, 2), (6, 1), (99, 99)):
            decoded = fields(record(kind=10, text=b'button.command', values=(HIGH_ID, MAX_ID, stage, result)))
            self.assertEqual(decoded['stage']['value'], stage)
            self.assertEqual(decoded['result']['value'], result)
        # OFF during the pattern remains attributed; OFF after completion has no ID.
        for source, press in ((2, HIGH_ID), (0, 0), (99, MAX_ID)):
            decoded = fields(record(kind=8, text=b'motor.applied', values=(0, source, press, 0)))
            self.assertEqual(decoded['motor']['value'], 0)
            self.assertEqual(decoded['source']['value'], source)
            self.assertEqual(decoded['press_id']['value'], press & 0xffffffff)

    def test_raw_database_export_and_timeline_preserve_device_evidence_and_unknown_tags(self):
        glove = [(label, kind, values) for label, kind, values, *_ in GLOVE_CASES]
        glove += [('motor.applied', 8, (0, 2, HIGH_ID, 0)),
                  ('button.command', 10, (HIGH_ID, MAX_ID, 6, 1)),
                  ('restart.request', 7, (2, MAX_ID, 0, 0)),
                  ('restart.wait', 7, (3, MAX_ID, 1805000, 1775000)),
                  ('restart.wait_cancel', 7, (3, MAX_ID, 40000, 0)),
                  ('restart.backoff_reset', 7, (MAX_ID, 600000, HIGH_ID, 0)),
                  ('future.button.trace', 8, (HIGH_ID, MAX_ID, 123, -7))]
        generator = [('wifi.offline.recovery', 7, (stage, 180000, 1, detail))
                     for stage, detail in ((1, 180000), (2, 10), (3, 0), (4, 0), (5, 0))]
        generator.append(('future.recovery.trace', 7, (HIGH_ID, MAX_ID, 123, -7)))
        with tempfile.TemporaryDirectory() as directory:
            for device, cases in (('iotglove', glove), ('HAS1_generator', generator)):
                with self.subTest(device=device):
                    path = Path(directory) / (device + '.sqlite3')
                    rows = [record(i, kind=kind, text=label.encode(), values=values)
                            for i, (label, kind, values) in enumerate(cases, 1)]
                    batch = parse_batch(TOPIC, payload(device=device, records=rows))
                    store = Store(path)
                    try:
                        store.save(batch)
                        store.save(batch)  # Retries preserve identity and original bytes.
                        self.assertEqual([row[0] for row in store.db.execute('SELECT raw FROM records ORDER BY seq')], rows)
                    finally:
                        store.close()
                    out = io.StringIO()
                    self.assertEqual(export(path, out)['count'], len(rows))
                    exported = [json.loads(line) for line in out.getvalue().splitlines()]
                    for raw, event in zip(rows, exported):
                        decoded = decode_record(raw)
                        self.assertEqual(event['raw_hex'], raw.hex())
                        self.assertEqual(event['values'], list(decoded.values))
                        self.assertEqual(event['named_values'], named_values(decoded, device))
                        timeline_event = device_event('store-1', MAC, RING, decoded.seq, device,
                                                      raw, event['received_utc'])
                        aligned = align_event(timeline_event, [])
                        self.assertEqual(aligned['raw'], raw.hex())
                        self.assertEqual(aligned['decoded_values'], event['named_values'])
                        self.assertIsNone(aligned['occurred_utc'])
                    self.assertEqual(exported[-1]['named_values'],
                                     {'schema': 'unknown', 'values': [HIGH_ID, MAX_ID, 123, -7]})


if __name__ == '__main__':
    unittest.main()
