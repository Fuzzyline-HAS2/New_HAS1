"""
시나리오: 부팅 홈잉 + 날개 위치 플래그 (wingsOpen)
설계: docs/superpowers/specs/2026-09-19-escape-wing-homing-design.md

재부팅 후 서버 상태가 activate이면 DataChanged가 EscapeOpen을 다시 불러
이미 열린 날개를 더 열던 문제. setup()에서 홈잉(EscapeClose)을 먼저 하고,
그 뒤로는 wings_open 플래그로 EscapeOpen 이중 실행을 막는다.
"""

from state_machine import EscapeMainSM


class TestBootHoming:
    def test_boot_activate_closes_before_opening(self):
        """activate 중 재부팅: 홈잉(EscapeClose)이 EscapeOpen보다 먼저 실행된다."""
        sm = EscapeMainSM()
        sm.boot("activate")
        events = sm.get_events()
        assert "EscapeClose" in events
        assert "EscapeOpen" in events
        assert events.index("EscapeClose") < events.index("EscapeOpen")

    def test_boot_activate_opens_exactly_once(self):
        sm = EscapeMainSM()
        sm.boot("activate")
        assert sm.get_events().count("EscapeOpen") == 1
        assert sm.wings_open is True

    def test_boot_ready_never_opens(self):
        """ready 중 재부팅: 홈잉만 하고 EscapeOpen은 없다."""
        sm = EscapeMainSM()
        sm.boot("ready")
        assert "EscapeOpen" not in sm.get_events()
        assert sm.wings_open is False


class TestWingsOpenGuard:
    def test_second_activate_skips_motor(self):
        """열린 채 ActivateFunc가 다시 불려도(MMMM 쓰기 실패 경로) 모터는 안 돈다."""
        sm = EscapeMainSM()
        sm.boot("activate")
        sm._activate_func()
        events = sm.get_events()
        assert events.count("EscapeOpen") == 1
        assert events.count("[MOTOR] EscapeOpen skipped") == 1
        assert sm.wings_open is True

    def test_reopen_allowed_after_close(self):
        """닫은 뒤에는 다시 열 수 있다."""
        sm = EscapeMainSM()
        sm.boot("activate")
        sm.data_changed(game_state="ready")
        assert sm.wings_open is False
        sm.data_changed(game_state="activate")
        assert sm.get_events().count("EscapeOpen") == 2
        assert sm.wings_open is True
