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

    def test_naive_modern_timestamp_uses_configured_offset(self):
        event = self.parse(b"<134>1 2026-10-24T12:00:00 ctrl app - - - test")
        self.assertEqual(event.event_utc, NOW.isoformat())
        self.assertEqual(event.time_quality, "naive_local_configured_offset")
        self.assertIn("naive_source_timestamp_configured_offset", event.details["timestamp_flags"])

    def test_omada_controller_client_online(self):
        event = self.parse(b'<142>1 2026-10-24 12:00:00 HAS1-Omada-HAS1 - - - [client:HAS1-Game-Light:18-DE-50-00-00-01] '
                           b'(IP: 192.168.30.101) went online on [ap:academy:AC-A7-F1-00-00-02] with SSID "HAS1-GUEST" on channel 6.')
        self.assertEqual(event.event_utc, NOW.isoformat())
        self.assertEqual(event.time_quality, "naive_local_configured_offset")
        self.assertEqual(event.event_type, "connected")
        self.assertEqual(event.device_mac, "18DE50000001")
        self.assertEqual(event.ap_mac, "ACA7F1000002")
        self.assertEqual(event.details["client_names"], ["HAS1-Game-Light"])
        self.assertEqual(event.details["ap_names"], ["academy"])
        self.assertEqual(event.details["explicit_fields"],
                         {"client_ip": ["192.168.30.101"], "ssid": ["HAS1-GUEST"], "channel": ["6"]})

    def test_omada_client_and_ap_offline(self):
        client = self.parse(b"<142>1 2026-10-24 12:00:00 h - - - [client:x:18-DE-50-00-00-01] went offline from [ap:a:AC-A7-F1-00-00-02].")
        self.assertEqual(client.event_type, "disconnected")
        ap = self.parse(b"<142>1 2026-10-24 12:00:00 h - - - [ap:academy:AC-A7-F1-00-00-02] went offline.")
        self.assertEqual(ap.event_type, "device_offline")
        online = self.parse(b"<142>1 2026-10-24 12:00:00 h - - - [ap:academy:AC-A7-F1-00-00-02] went online.")
        self.assertEqual(online.event_type, "device_online")

    def test_omada_controller_dhcp(self):
        event = self.parse(b"<134>1 2026-10-24 12:00:00 HAS1-Omada-HAS1 - - - DHCP Server allocated IP address "
                           b"192.168.30.101 for the [client:18-DE-50-00-00-01].")
        self.assertEqual(event.event_type, "dhcp_lease")
        self.assertEqual(event.device_mac, "18DE50000001")
        self.assertEqual(event.details["explicit_fields"], {"allocated_ip": ["192.168.30.101"]})

    def test_gateway_legacy_header_takes_year_from_body(self):
        event = self.parse(b"<13>Oct 24 12:00:00 ER7212PC: 2026-10-24 12:00:00 dhcp_server<5>: [OMADA]DHCP Server "
                           b"allocated IP address 192.168.30.104 for the [client:bc:35:1e:00:00:03].")
        self.assertEqual(event.event_utc, NOW.isoformat())
        self.assertEqual(event.time_quality, "naive_local_configured_offset")
        self.assertNotIn("year_ambiguous", event.details["timestamp_flags"])
        self.assertEqual(event.event_type, "dhcp_lease")
        self.assertEqual(event.device_mac, "BC351E000003")

    def test_gateway_legacy_body_date_must_match_header(self):
        event = self.parse(b"<13>Oct 24 12:00:00 gw: 2025-01-01 00:00:00 unrelated")
        self.assertEqual(event.time_quality, "estimated_year_configured_offset")

    def test_ap_traffic_log_records(self):
        raw = (b"<6>Oct 24 12:00:01 192.168.30.26 [1792800000.740301419] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 "
               b"IP SRC=172.30.1.110 IP DST=172.30.1.43 IP proto=6 SPT=80 DPT=57667\n\n"
               b"[1792800000.830301419] AP MAC=ac:a7:f1:00:00:02 MAC SRC=2c:bc:bb:00:00:05 IP SRC=172.30.1.128 "
               b"IP DST=172.30.1.43 IP proto=6 SPT=59918 DPT=80\n\n[1792800000.930301419] AP MAC=ac:a7:f1:00:00:02 ")
        event = self.parse(raw, datetime(2026, 10, 24, 3, 0, 1, tzinfo=timezone.utc))
        self.assertEqual(event.event_type, "traffic_log")
        self.assertEqual(event.time_quality, "source_epoch_unverified")
        self.assertEqual(event.event_utc, datetime.fromtimestamp(1792800000.740301, timezone.utc).isoformat())
        self.assertNotIn("year_ambiguous", event.details["timestamp_flags"])
        self.assertNotIn("assumed_utc_offset_minutes", event.details)
        self.assertEqual(event.ap_mac, "ACA7F1000002")
        self.assertIsNone(event.device_mac)
        self.assertEqual(event.details["traffic_record_count"], 2)
        self.assertEqual(event.details["traffic_src_macs"], ["A4F00F000004", "2CBCBB000005"])
        self.assertEqual(event.details["traffic_first_utc"], datetime.fromtimestamp(1792800000.740301, timezone.utc).isoformat())
        self.assertEqual(event.details["traffic_last_utc"], datetime.fromtimestamp(1792800000.830301, timezone.utc).isoformat())
        self.assertEqual(event.details["traffic_src_ports"], [80, 59918])
        self.assertEqual(event.details["traffic_dst_ports"], [57667, 80])
        self.assertFalse(event.details["traffic_summary_truncated"])
        self.assertNotIn("traffic_records", event.details)

    def test_traffic_summary_is_bounded(self):
        records = b"".join(b"[1792800000.1] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 IP SRC=1.1.1.1 "
                           b"IP DST=2.2.2.2 IP proto=6 SPT=%d DPT=80\n" % (40000 + n) for n in range(40))
        event = self.parse(b"<6>Oct 24 12:00:00 ap " + records)
        self.assertEqual(event.details["traffic_record_count"], 40)
        self.assertEqual(len(event.details["traffic_src_ports"]), 32)
        self.assertEqual(event.details["traffic_dst_ports"], [80])
        self.assertTrue(event.details["traffic_summary_truncated"])
        no_ports = self.parse(b"<6>Oct 24 12:00:00 ap [1792800000.1] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 "
                              b"IP SRC=1.1.1.1 IP DST=2.2.2.2 IP proto=17\n")
        self.assertEqual(no_ports.details["traffic_src_ports"], [])

    def test_traffic_record_cut_at_datagram_end_is_not_stored(self):
        complete = b"[1792800000.1] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 IP SRC=1.1.1.1 IP DST=2.2.2.2 IP proto=6 SPT=80 DPT=57667\n"
        cut = b"[1792800000.2] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 IP SRC=1.1.1.1 IP DST=2.2.2.2 IP proto=6 SPT=80 DPT=57"
        event = self.parse(b"<6>Oct 24 12:00:00 ap " + complete + cut)
        self.assertEqual(event.details["traffic_record_count"], 1)
        self.assertEqual(event.details["traffic_dst_ports"], [57667])
        self.assertEqual(event.details["traffic_partial_records"], 1)
        cut_ip = self.parse(b"<6>Oct 24 12:00:00 ap " + complete + cut.split(b" IP DST")[0][:-1])
        self.assertEqual(cut_ip.details["traffic_record_count"], 1)

    def test_implausible_traffic_epoch_keeps_header_time(self):
        event = self.parse(b"<6>Oct 24 12:00:00 ap [123456789.1] AP MAC=ac:a7:f1:00:00:02 MAC SRC=a4:f0:0f:00:00:04 "
                           b"IP SRC=1.1.1.1 IP DST=2.2.2.2 IP proto=6\n")
        self.assertEqual(event.event_utc, NOW.isoformat())
        self.assertEqual(event.time_quality, "estimated_year_configured_offset")
        self.assertIn("traffic_epoch_implausible", event.details["timestamp_flags"])

    def test_dhcp_requires_allocation_sentence(self):
        event = self.parse(b"<134>1 2026-10-24 12:00:00 h - - - [client:18-DE-50-00-00-01] DHCP request; client connected, allocated later")
        self.assertEqual(event.event_type, "connected")

    def test_label_floods_stay_within_telemetry_limit(self):
        names = b"".join(b"[client:XY" + b"\xff" * 124 + b":aa:bb:cc:dd:ee:%02x]" % n for n in range(53))
        reasons = b"[reason:" + b"\xff" * 120 + b"]"
        for raw in (b"<134>1 - h - - - " + names, b"<134>1 - h - - - " + reasons * 60):
            event = self.parse(raw)
            to_telemetry(event)  # must not raise "fields exceed bounded size"
        self.assertLessEqual(len(event.details["explicit_fields"]["reason"]), 8)

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
