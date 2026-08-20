"""
시나리오 1: setting → ready → activate 상태 전환 테스트
DataChanged()가 상태 변화를 올바르게 감지하고
각 Func()가 정확한 동작을 수행하는지 검증합니다.
"""

import pytest
from state_machine import EscapeMainSM


class TestSettingState:
    def setup_method(self):
        self.sm = EscapeMainSM()
        self.sm.data_changed(game_state="setting")
        self.events = self.sm.get_events()

    def test_setting_event_printed(self):
        assert "SETTING" in self.events

    def test_relay_is_off_in_setting(self):
        assert self.sm.relay_high is True

    def test_escape_close_called_in_setting(self):
        assert "EscapeClose" in self.events

    def test_game_timer_disabled_in_setting(self):
        assert self.sm._timer_enabled is False

    def test_mode_is_wait_in_setting(self):
        assert self.sm._mode == "wait"

    def test_tagcount_not_called_in_setting(self):
        """setting 상태에서 GameTimer tick이 와도 TagCount는 실행 안됨."""
        self.sm.set_tags(True, True, True)
        self.sm.tick_game_timer()
        assert not self.sm.get_sent()


class TestReadyState:
    def setup_method(self):
        self.sm = EscapeMainSM()
        self.sm.data_changed(game_state="setting")
        self.sm.data_changed(game_state="ready")
        self.sm.clear_events()
        # ready로 전환 후 이벤트만 보기 위해 재실행
        self.sm2 = EscapeMainSM()
        self.sm2.data_changed(game_state="setting")
        self.sm2.clear_events()
        self.sm2.data_changed(game_state="ready")
        self.events = self.sm2.get_events()

    def test_ready_event_printed(self):
        assert "READY" in self.events

    def test_relay_is_off_in_ready(self):
        assert self.sm2.relay_high is True

    def test_escape_close_called_in_ready(self):
        assert "EscapeClose" in self.events

    def test_mode_is_wait_in_ready(self):
        assert self.sm2._mode == "wait"


class TestActivateState:
    def setup_method(self):
        self.sm = EscapeMainSM()
        self.sm.data_changed(game_state="setting")
        self.sm.data_changed(game_state="ready")
        self.sm.clear_events()
        self.sm.data_changed(game_state="activate")
        self.events = self.sm.get_events()

    def test_activate_event_printed(self):
        assert "ACTIVATE" in self.events

    def test_relay_is_on_in_activate(self):
        assert self.sm.relay_high is False

    def test_escape_open_called_in_activate(self):
        assert "EscapeOpen" in self.events

    def test_game_timer_enabled_in_activate(self):
        assert self.sm._timer_enabled is True

    def test_mode_is_tagcount_in_activate(self):
        assert self.sm._mode == "tagcount"


class TestChangeDetection:
    def test_same_state_does_not_retrigger(self):
        """같은 game_state를 두 번 보내도 함수 재실행 없음 (cur 비교 로직)."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.clear_events()
        sm.data_changed(game_state="setting")
        assert "SETTING" not in sm.get_events()

    def test_player_win_triggers_close_and_blue(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm.clear_events()
        sm.data_changed(device_state="player_win")
        events = sm.get_events()
        assert "AllNeoOn(BLUE)" in events
        assert "EscapeClose" in events

    def test_player_win_same_state_no_retrigger(self):
        sm = EscapeMainSM()
        sm.data_changed(device_state="player_win")
        sm.clear_events()
        sm.data_changed(device_state="player_win")
        assert "EscapeClose" not in sm.get_events()


class TestFullGameFlow:
    def test_setting_ready_activate_sequence(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.data_changed(game_state="ready")
        sm.data_changed(game_state="activate")
        events = sm.get_events()
        assert "SETTING" in events
        assert "READY" in events
        assert "ACTIVATE" in events
        # 순서 검증
        assert events.index("SETTING") < events.index("READY") < events.index("ACTIVATE")

    def test_activate_then_reset_to_setting(self):
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm.data_changed(game_state="setting")
        assert sm._mode == "wait"
        assert sm._timer_enabled is False
        assert sm.relay_high is True
