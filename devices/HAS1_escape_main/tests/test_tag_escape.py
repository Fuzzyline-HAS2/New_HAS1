"""
시나리오 2: Beetle 태그 수신 → escape 전송 테스트
Beetle로부터 태그가 감지될 때 TagCount() 로직이
올바르게 동작하는지 검증합니다.
"""

import pytest
from state_machine import EscapeMainSM


@pytest.fixture
def activated_sm():
    """activate 상태로 준비된 SM."""
    sm = EscapeMainSM()
    sm.data_changed(game_state="setting")
    sm.data_changed(game_state="activate")
    sm.clear_events()
    sm.clear_sent()
    return sm


class TestTagCountLogic:
    def test_1_tag_no_escape(self, activated_sm):
        activated_sm.set_tags(True, False, False)
        activated_sm.tick_game_timer()
        assert not activated_sm.get_sent()
        assert "Tag 1 detected" in activated_sm.get_events()

    def test_2_tags_no_escape(self, activated_sm):
        activated_sm.set_tags(True, True, False)
        activated_sm.tick_game_timer()
        assert not activated_sm.get_sent()
        assert "Tag 2 detected" in activated_sm.get_events()

    def test_3_tags_sends_escape(self, activated_sm):
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        sent = activated_sm.get_sent()
        assert len(sent) == 1
        assert sent[0]["column"] == "device_state"
        assert sent[0]["value"] == "escape"

    def test_3_tags_escape_device_name(self, activated_sm):
        """Send 명령의 key가 device_name인지 확인."""
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert activated_sm.get_sent()[0]["key"] == "updated_escape_main"

    def test_escape_sends_only_once(self, activated_sm):
        """escape는 1번만 전송돼야 함."""
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert len(activated_sm.get_sent()) == 1


class TestTagStateReset:
    def test_tag_state_reset_after_tick(self, activated_sm):
        """TagCount() 실행 후 tagState[] 초기화 확인."""
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert activated_sm.tag_state == [False, False, False]

    def test_tag_state_reset_after_partial_tag(self, activated_sm):
        activated_sm.set_tags(True, False, False)
        activated_sm.tick_game_timer()
        assert activated_sm.tag_state == [False, False, False]


class TestModeTransitionOnEscape:
    def test_mode_becomes_wait_after_escape(self, activated_sm):
        """escape 전송 후 ptrCurrentMode = WaitFunc 전환 확인."""
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert activated_sm._mode == "wait"
        assert activated_sm._timer_enabled is False

    def test_no_more_tagcount_after_escape(self, activated_sm):
        """escape 이후 GameTimer tick이 와도 TagCount 실행 안됨."""
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        activated_sm.clear_sent()
        activated_sm.clear_events()
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert not activated_sm.get_sent()


class TestTagCountAccumulation:
    def test_incremental_tags(self, activated_sm):
        """태그를 1개 → 2개 → 3개 순차 감지."""
        activated_sm.set_tags(True, False, False)
        activated_sm.tick_game_timer()
        assert "Tag 1 detected" in activated_sm.get_events()

        activated_sm.clear_events()
        activated_sm.set_tags(True, True, False)
        activated_sm.tick_game_timer()
        assert "Tag 2 detected" in activated_sm.get_events()

        activated_sm.clear_events()
        activated_sm.set_tags(True, True, True)
        activated_sm.tick_game_timer()
        assert "Escape Activate" in activated_sm.get_events()
        assert activated_sm.get_sent()
