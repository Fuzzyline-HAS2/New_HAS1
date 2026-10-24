import json
from pathlib import Path
import socket
import sys
import threading
import time
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gateway import GatewayReceiver, parse_gateway


def packet(**overrides):
    data = dict(schema_version=1, site_id="store-1", source="game_gateway",
                source_id="store-1-php-gateway", event_id="gateway-1",
                occurred_utc="2026-10-24T09:00:00Z", observed_utc="2000-01-01T00:00:00Z",
                clock_quality="measured", uncertainty_ms=0,
                event_type="gateway_request_finished", fields={"proxy_result": "failed"})
    data.update(overrides)
    return json.dumps(data).encode()


class GatewayTests(unittest.TestCase):
    def parse(self, data):
        return parse_gateway(data, site_id="store-1", source_id="store-1-php-gateway",
                             observed_utc="2026-10-24T09:00:01Z")

    def test_actual_arrival_and_unverified_clock_override_sender(self):
        event = self.parse(packet())
        self.assertIn("2026-10-24T09:00:01", event.observed_utc)
        self.assertEqual(event.clock_quality, "unverified")
        self.assertIsNone(event.uncertainty_ms)

    def test_malformed_oversize_schema_and_provenance_rejected(self):
        for data in [b"{", b"x" * 2049, packet(site_id="store-2"), packet(source="game_server"),
                     packet(source_id="another"), packet(schema_version=2),
                     packet(event_type="unknown"), packet(occurred_utc="no-time")]:
            with self.subTest(data=data[:60]), self.assertRaises(ValueError):
                self.parse(data)

    def test_no_lan_bind(self):
        with self.assertRaises(ValueError):
            GatewayReceiver(host="0.0.0.0", site_id="store-1", source_id="x", on_event=lambda e: None)

    def test_real_udp_independent_callback_and_bounded_overflow(self):
        gate, entered = threading.Event(), threading.Event()
        events, close_threads = [], []
        def consume(event):
            entered.set()
            gate.wait(3)
            events.append(event)
        receiver = GatewayReceiver(port=0, site_id="store-1", source_id="store-1-php-gateway",
                                   queue_capacity=1, on_event=consume,
                                   on_close=lambda: close_threads.append(threading.current_thread().name))
        receiver.start()
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sender.sendto(packet(), ("127.0.0.1", receiver.port))
            self.assertTrue(entered.wait(2))
            for _ in range(20): sender.sendto(packet(), ("127.0.0.1", receiver.port))
            deadline = time.monotonic() + 2
            while receiver.stats()["received"] < 21 and time.monotonic() < deadline: time.sleep(.01)
            self.assertGreater(receiver.stats()["overflow"], 0)
            self.assertLessEqual(receiver.stats()["queue_depth"], 1)
            self.assertFalse(receiver.stop(.01))
            gate.set()
            self.assertTrue(receiver.stop(2))
            self.assertEqual(close_threads, ["gateway-store"])
            self.assertLessEqual(len(events), 2)
        finally:
            gate.set(); receiver.stop(3); sender.close()

    def test_invalid_input_and_callback_failure_counted(self):
        def fail(_): raise OSError("disk full")
        receiver = GatewayReceiver(port=0, site_id="store-1", source_id="store-1-php-gateway", on_event=fail)
        receiver.start()
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            for data in [b"bad", packet(), packet(source_id="wrong")]:
                sender.sendto(data, ("127.0.0.1", receiver.port))
            deadline = time.monotonic() + 2
            while receiver.stats()["callback_errors"] < 1 and time.monotonic() < deadline: time.sleep(.01)
        finally: receiver.stop(2); sender.close()
        self.assertEqual(receiver.stats()["invalid"], 2)
        self.assertEqual(receiver.stats()["callback_errors"], 1)
        self.assertFalse(receiver.is_receiving())

if __name__ == "__main__": unittest.main()
