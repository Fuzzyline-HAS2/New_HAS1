from pathlib import Path
import socket
import sys
import threading
import unittest
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from omada import OmadaReceiver, parse_syslog, to_telemetry

NOW = datetime(2026, 10, 24, 3, 0, tzinfo=timezone.utc)


class ParserTests(unittest.TestCase):
    def parse(self, data, now=NOW, **kwargs):
        return parse_syslog(data, "172.30.1.1", now, "site1", **kwargs)

    def test_rfc5424_explicit_offset(self):
        event = self.parse(b"<134>1 2026-10-24T12:00:00+09:00 ctrl app - - - Client AA:BB:CC:DD:EE:FF disconnected")
        self.assertEqual(event.event_utc, "2026-10-24T03:00:00+00:00")
        self.assertEqual(event.time_quality, "source_offset_unverified")
        self.assertEqual(event.device_mac, "AABBCCDDEEFF")
        self.assertEqual(event.event_type, "disconnected")

    def test_utc_fraction(self):
        event = self.parse(b"<134>1 2026-10-24T03:00:00.123Z ctrl app - - - test")
        self.assertEqual(event.event_utc, "2026-10-24T03:00:00.123000+00:00")

    def test_naive_modern_timestamp_is_not_guessed(self):
        event = self.parse(b"<134>1 2026-10-24T12:00:00 ctrl app - - - test")
        self.assertIsNone(event.event_utc)
        self.assertIn("invalid_or_naive_source_timestamp", event.details["timestamp_flags"])

    def test_nil_timestamp(self):
        self.assertIsNone(self.parse(b"<134>1 - ctrl app - - - test").event_utc)

    def test_invalid_date_retained(self):
        raw = b"<134>1 2026-02-30T03:00:00Z ctrl app - - - test"
        event = self.parse(raw)
        self.assertIsNone(event.event_utc)
        self.assertEqual(event.raw, raw)

    def test_legacy_kst(self):
        event = self.parse(b"Oct 24 12:00:00 controller message")
        self.assertEqual(event.event_utc, NOW.isoformat())
        self.assertEqual(event.time_quality, "estimated_year_configured_offset")
        self.assertIn("year_ambiguous", event.details["timestamp_flags"])

    def test_year_boundary_previous(self):
        now = datetime(2026, 12, 31, 15, 0, 3, tzinfo=timezone.utc)
        event = self.parse(b"Dec 31 23:59:59 controller test", now)
        self.assertEqual(event.event_utc, "2026-12-31T14:59:59+00:00")

    def test_year_boundary_next(self):
        now = datetime(2026, 12, 31, 14, 59, 58, tzinfo=timezone.utc)
        event = self.parse(b"Jan  1 00:00:00 controller test", now)
        self.assertEqual(event.event_utc, "2026-12-31T15:00:00+00:00")

    def test_delayed_source_time_not_rewritten(self):
        event = self.parse(b"<134>1 2026-10-23T03:00:00Z c a - - - test")
        self.assertEqual(event.event_utc, "2026-10-23T03:00:00+00:00")
        self.assertIn("delayed_or_clock_skewed", event.details["timestamp_flags"])
        self.assertEqual(event.observed_utc, NOW.isoformat())

    def test_future_clock_skew(self):
        event = self.parse(b"<134>1 2026-10-24T03:10:00Z c a - - - test")
        self.assertIn("source_time_in_future", event.details["timestamp_flags"])

    def test_roaming_explicit_client_and_aps(self):
        event = self.parse(b"Oct 24 12:00:00 ctrl [client:glove:aa-bb-cc-dd-ee-ff] is roaming from [ap:Old:11-22-33-44-55-66] to [ap:New:22-33-44-55-66-77]")
        self.assertEqual(event.device_mac, "AABBCCDDEEFF")
        self.assertIsNone(event.ap_mac)
        self.assertEqual(event.details["ap_macs"], ["112233445566", "223344556677"])
        self.assertEqual(event.event_type, "roaming")

    def test_ambiguous_client_not_assigned(self):
        event = self.parse(b"Client aa:bb:cc:dd:ee:ff and Client 11:22:33:44:55:66 connected")
        self.assertIsNone(event.device_mac)
        self.assertTrue(event.details["ambiguous_client"])

    def test_unlabelled_mac_not_assigned(self):
        event = self.parse(b"AP aa:bb:cc:dd:ee:ff connected")
        self.assertIsNone(event.device_mac)
        self.assertEqual(event.ap_mac, "AABBCCDDEEFF")

    def test_explicit_payload_site_is_preserved_unverified(self):
        event = self.parse(b"Oct 24 12:00:00 ctrl [site:other-store] Client aa:bb:cc:dd:ee:ff disconnected")
        self.assertEqual(event.site_id, "site1")
        self.assertEqual(event.details["declared_site_names"], ["other-store"])
        self.assertEqual(event.details["site_binding"], "configured_sender_payload_site_unverified")

    def test_failed_connect_not_connection_success(self):
        event = self.parse(b"Client aa:bb:cc:dd:ee:ff failed to connect")
        self.assertEqual(event.event_type, "connection_failed")

    def test_unknown_binary_raw_preserved(self):
        raw = b"unexpected\xff\x00binary"
        event = self.parse(raw)
        self.assertEqual(event.raw, raw)
        self.assertEqual(event.event_type, "unknown")
        self.assertIsNone(event.event_utc)

    def test_oversize_explicit_truncation(self):
        event = self.parse(b"x" * 9000)
        self.assertEqual(len(event.raw), 8192)
        self.assertTrue(event.details["truncated"])
        self.assertEqual(event.details["received_bytes"], 9000)

    def test_invalid_observer_or_site_rejected(self):
        with self.assertRaises(ValueError):
            self.parse(b"test", datetime(2026, 1, 1))
        with self.assertRaises(ValueError):
            parse_syslog(b"test", "172.30.1.1", NOW, "")


class AdapterTests(unittest.TestCase):
    def test_raw_binary_roundtrip_and_no_false_clock_accuracy(self):
        import base64
        source = parse_syslog(b"<134>1 2026-10-24T03:00:00Z c a - - - \xff\x00", "172.30.1.1", NOW, "store-1")
        event = to_telemetry(source)
        self.assertEqual(base64.b64decode(event.raw), source.raw)
        self.assertEqual(event.clock_quality, "source_offset_unverified")
        self.assertIsNone(event.uncertainty_ms)
        self.assertEqual(event.source_id, "172.30.1.1")

    def test_repeated_identical_arrivals_keep_separate_identity(self):
        source = parse_syslog(b"repeat", "172.30.1.1", NOW, "store-1")
        self.assertNotEqual(to_telemetry(source).event_id, to_telemetry(source).event_id)

    def test_maximum_datagram_fits_telemetry_bound(self):
        import base64
        source = parse_syslog(b"x" * 9000, "172.30.1.1", NOW, "store-1")
        event = to_telemetry(source)
        self.assertEqual(len(base64.b64decode(event.raw)), 8192)
        self.assertTrue(event.fields["truncated"])


class ReceiverTests(unittest.TestCase):
    def receiver(self, callback=lambda event: None, **kwargs):
        return OmadaReceiver("127.0.0.1", 0, {"127.0.0.1": "site1"}, callback, **kwargs)

    def test_source_allowlist_and_explicit_site(self):
        receiver = self.receiver()
        self.assertFalse(receiver.submit(b"test", "127.0.0.2", NOW))
        self.assertTrue(receiver.submit(b"test", "127.0.0.1", NOW))
        self.assertEqual(receiver.queue.get_nowait().site_id, "site1")
        self.assertEqual(receiver.stats()["filtered"], 1)
        self.assertTrue(receiver.stop())

    def test_queue_overflow_nonblocking(self):
        receiver = self.receiver(queue_capacity=1)
        self.assertTrue(receiver.submit(b"one", "127.0.0.1", NOW))
        self.assertFalse(receiver.submit(b"two", "127.0.0.1", NOW))
        self.assertEqual(receiver.stats()["overflow"], 1)
        self.assertEqual(receiver.stats()["queue_size"], 1)
        receiver.stop()

    def test_live_udp_callback_and_close_on_same_thread(self):
        received = []
        closed = []
        ready = threading.Event()
        def callback(event):
            received.append((event, threading.get_ident()))
            ready.set()
        receiver = self.receiver(callback, on_close=lambda: closed.append(threading.get_ident()))
        receiver.start()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                sender.sendto(b"Oct 24 12:00:00 ctrl Client aa-bb-cc-dd-ee-ff connected", ("127.0.0.1", receiver.port))
            self.assertTrue(ready.wait(2))
        finally:
            self.assertTrue(receiver.stop())
        self.assertEqual(received[0][0].device_mac, "AABBCCDDEEFF")
        self.assertEqual(closed, [received[0][1]])
        self.assertFalse(receiver.submit(b"late", "127.0.0.1"))
        with self.assertRaises(RuntimeError):
            receiver.start()

    def test_stop_drains_queued_items(self):
        received = []
        receiver = self.receiver(received.append)
        for number in range(10):
            receiver.submit(str(number).encode(), "127.0.0.1", NOW)
        receiver.start()
        self.assertTrue(receiver.stop())
        self.assertEqual(len(received), 10)
        self.assertEqual(receiver.stats()["stored"], 10)

    def test_callback_failure_is_counted_and_next_event_runs(self):
        received = []
        def callback(event):
            received.append(event.raw)
            if event.raw == b"fail":
                raise OSError("test db failure")
        receiver = self.receiver(callback)
        receiver.submit(b"fail", "127.0.0.1", NOW)
        receiver.submit(b"next", "127.0.0.1", NOW)
        receiver.start()
        self.assertTrue(receiver.stop())
        self.assertEqual(received, [b"fail", b"next"])
        self.assertEqual(receiver.stats()["callback_errors"], 1)
        self.assertEqual(receiver.stats()["stored"], 1)

    def test_blocked_callback_stop_reports_timeout(self):
        entered, release = threading.Event(), threading.Event()
        def callback(event):
            entered.set()
            release.wait(2)
        receiver = self.receiver(callback)
        receiver.submit(b"test", "127.0.0.1", NOW)
        receiver.start()
        try:
            self.assertTrue(entered.wait(1))
            self.assertFalse(receiver.stop(0.01))
        finally:
            release.set()
            self.assertTrue(receiver.stop(2))

    def test_duplicate_listener_fails_without_reuse(self):
        receiver = self.receiver()
        receiver.start()
        second = OmadaReceiver("127.0.0.1", receiver.port, {"127.0.0.1": "site1"}, lambda event: None)
        try:
            with self.assertRaises(OSError):
                second.start()
        finally:
            receiver.stop()
            second.stop()

    def test_invalid_configuration(self):
        for args in ({"source_sites": {}}, {"source_sites": {"127.0.0.1": ""}},
                     {"queue_capacity": 0}, {"max_datagram": 1}, {"utc_offset_minutes": 2000}):
            config = {"bind_host": "127.0.0.1", "port": 0, "source_sites": {"127.0.0.1": "site1"}, "on_event": lambda event: None}
            config.update(args)
            with self.assertRaises(ValueError):
                OmadaReceiver(**config)


if __name__ == "__main__":
    unittest.main()
