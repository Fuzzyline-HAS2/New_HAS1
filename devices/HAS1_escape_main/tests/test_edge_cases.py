"""
시나리오 4: 경계 조건 및 예외 상황 테스트
정상 흐름 밖의 입력이 들어왔을 때 시스템이
안전하게 동작하는지 검증합니다.
"""

import pytest
from state_machine import EscapeMainSM


class TestRapidStateChanges:
    def test_rapid_setting_ready_activate(self):
        """빠른 연속 상태 전환 시 최종 상태가 올바른지."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.data_changed(game_state="ready")
        sm.data_changed(game_state="activate")
        sm.data_changed(game_state="setting")
        assert sm.game_state == "setting"
        assert sm._mode == "wait"
        assert sm._timer_enabled is False

    def test_all_three_funcs_run_in_sequence(self):
        """각 상태에서 해당 함수가 실행됐는지."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.data_changed(game_state="ready")
        sm.data_changed(game_state="activate")
        events = sm.get_events()
        assert "SETTING" in events
        assert "READY" in events
        assert "ACTIVATE" in events


class TestGameTimerGating:
    def test_timer_not_active_in_setting(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.set_tags(True, True, True)
        sm.tick_game_timer()
        assert not sm.get_sent()

    def test_timer_not_active_in_ready(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="ready")
        sm.set_tags(True, True, True)
        sm.tick_game_timer()
        assert not sm.get_sent()

    def test_timer_active_only_in_activate(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm.set_tags(True, True, True)
        sm.tick_game_timer()
        assert sm.get_sent()


class TestRelayStateConsistency:
    def test_relay_off_after_setting(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        assert sm.relay_high is True  # HIGH = 릴레이 OFF

    def test_relay_off_after_ready(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="ready")
        assert sm.relay_high is True

    def test_relay_on_after_activate(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        assert sm.relay_high is False  # LOW = 릴레이 ON (모터 전원)

    def test_relay_restored_after_activate_to_setting(self):
        """activate → setting 전환 시 릴레이 원복."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        assert sm.relay_high is False
        sm.data_changed(game_state="setting")
        assert sm.relay_high is True


class TestTagCntBounds:
    def test_tag_cnt_never_exceeds_3(self):
        """tagCnt가 3을 초과하지 않는지 (LOGIC_TAG_01 대응)."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        # 최대 3개 태그만 감지 가능한 구조
        sm.set_tags(True, True, True)
        sm.tick_game_timer()
        assert sm.tag_cnt <= 3

    def test_tag_cnt_not_negative(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm.set_tags(False, False, False)
        sm.tick_game_timer()
        assert sm.tag_cnt >= 0


class TestDeviceStateIndependent:
    def test_game_state_and_device_state_independent(self):
        """game_state와 device_state가 서로 독립적으로 변경됨."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate", device_state="ready")
        assert sm.game_state == "activate"
        assert sm.device_state == "ready"

    def test_unknown_game_state_ignored(self):
        """허용되지 않은 game_state는 아무 함수도 호출하지 않음."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="unknown_state")
        events = sm.get_events()
        assert "SETTING" not in events
        assert "READY" not in events
        assert "ACTIVATE" not in events
