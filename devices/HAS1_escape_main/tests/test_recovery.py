"""
시나리오 5: 런타임 복구 및 안정 상태 관리 테스트
error_recovery.ino의 핵심 복구 로직을 Python 상태머신으로 검증합니다.

테스트 클래스:
  TestBadEventRecovery   - bad event 3회 → UART recovery 실행
  TestFaultRetryable     - fault 이후 재시도 허용, 성공 시 해제, 재실패 시 재래치
  TestMotorFaultSafeStop - motor fault → safe stop
"""

import pytest
from state_machine import EscapeMainSM


# ─────────────────────────────────────────────────────────────────────────────
# [1] bad event 3회 누적 → UART recovery 실행
#     Beetle silence가 아닌 이벤트 기반 복구의 핵심 검증.
# ─────────────────────────────────────────────────────────────────────────────
class TestBadEventRecovery:
    def test_uart_recovery_triggered_after_3_bad_event_cycles(self):
        """bad event가 3 사이클에 걸쳐 누적되면 UART 재초기화가 실행된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")

        sm.receive_bad_event("invalid_cmd")
        sm.handle_runtime_recovery()   # streak=1

        sm.receive_bad_event("format_error")
        sm.handle_runtime_recovery()   # streak=2

        sm.receive_bad_event("tag_parse")
        sm.handle_runtime_recovery()   # streak=3 → UART recovery

        assert sm.beetle_recover_attempts >= 1

    def test_uart_recovery_event_logged(self):
        """UART 재초기화 시 이벤트 로그가 남는다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")

        for kind in ("invalid_cmd", "format_error", "tag_parse"):
            sm.receive_bad_event(kind)
            sm.handle_runtime_recovery()

        assert any("[RECOVERY] UART reset" in e for e in sm.get_events())

    def test_uart_recovery_resets_error_counters(self):
        """UART 재초기화 후 오류 카운터가 초기화된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")

        for kind in ("invalid_cmd", "format_error", "tag_parse"):
            sm.receive_bad_event(kind)
            sm.handle_runtime_recovery()

        assert sm.invalid_cmd_count == 0
        assert sm.packet_format_error_count == 0
        assert sm.tag_parse_error_count == 0
        assert sm.beetle_bad_event_streak == 0

    def test_silence_does_not_trigger_recovery(self):
        """Beetle silence만으로는 UART recovery가 실행되지 않는다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")

        for _ in range(10):
            sm.handle_runtime_recovery()  # 아무 bad event 없이 반복

        assert sm.beetle_recover_attempts == 0

    def test_single_bad_event_does_not_trigger_recovery(self):
        """bad event가 2회 이하이면 UART recovery가 실행되지 않는다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")

        sm.receive_bad_event("invalid_cmd")
        sm.handle_runtime_recovery()   # streak=1
        sm.receive_bad_event("format_error")
        sm.handle_runtime_recovery()   # streak=2

        assert sm.beetle_recover_attempts == 0


# ─────────────────────────────────────────────────────────────────────────────
# [2] fault 이후 재시도 허용
#     fault는 영구 차단이 아닌 현재 시도 실패 기록 + safe stop.
#     다음 전이 시작 시 ClearSystemFault()로 해제 후 재시도 가능.
#     재시도 중 또 timeout 나면 HandleRuntimeRecovery가 다시 래치.
# ─────────────────────────────────────────────────────────────────────────────
class TestFaultRetryable:
    def _latch_via_motor_timeout(self, sm: EscapeMainSM):
        """모터 timeout으로 systemFaultLatched를 True로 만드는 헬퍼."""
        sm.data_changed(game_state="setting")
        sm._motor_close_timeout = True
        sm.data_changed(game_state="ready")   # early return
        sm.handle_runtime_recovery()           # LatchSystemFault

    def test_activate_not_blocked_after_fault(self):
        """fault 이후 activate가 차단되지 않고 실행된다."""
        sm = EscapeMainSM()
        self._latch_via_motor_timeout(sm)
        assert sm.system_fault_latched is True

        sm.clear_events()
        sm.data_changed(game_state="activate")
        assert not any("blocked" in e.lower() for e in sm.get_events())

    def test_fault_clears_on_successful_activate(self):
        """fault 이후 activate가 성공하면 systemFaultLatched가 False가 된다."""
        sm = EscapeMainSM()
        self._latch_via_motor_timeout(sm)
        sm.data_changed(game_state="activate")
        assert sm.system_fault_latched is False

    def test_fault_clears_at_start_of_setting_transition(self):
        """setting 전이 시작 시 기존 fault가 해제된다."""
        sm = EscapeMainSM()
        sm.system_fault_latched = True
        sm.system_fault_reason = "previous fault"
        sm.data_changed(game_state="setting")
        assert sm.system_fault_latched is False

    def test_fault_clears_at_start_of_ready_transition(self):
        """ready 전이 시작 시 기존 fault가 해제된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="setting")
        sm.system_fault_latched = True
        sm.data_changed(game_state="ready")
        assert sm.system_fault_latched is False

    def test_fault_relatches_on_second_motor_timeout(self):
        """재시도 중 또 timeout 나면 fault가 다시 래치된다."""
        sm = EscapeMainSM()
        self._latch_via_motor_timeout(sm)

        sm._motor_open_timeout = True           # 재시도 중 또 timeout
        sm.data_changed(game_state="activate")  # ClearSystemFault → timeout → early return
        sm.handle_runtime_recovery()            # LatchSystemFault again
        assert sm.system_fault_latched is True


# ─────────────────────────────────────────────────────────────────────────────
# [3] motor fault → safe stop
#     통신 오류 복구와 기구 fault 처리를 명확히 분리.
# ─────────────────────────────────────────────────────────────────────────────
class TestMotorFaultSafeStop:
    def test_motor_close_timeout_triggers_safe_stop(self):
        """EscapeClose timeout 시 systemFaultLatched=True가 된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm._motor_close_timeout = True
        sm.handle_runtime_recovery()
        assert sm.system_fault_latched is True

    def test_motor_open_timeout_triggers_safe_stop(self):
        """EscapeOpen timeout 시 systemFaultLatched=True가 된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm._motor_open_timeout = True
        sm.handle_runtime_recovery()
        assert sm.system_fault_latched is True

    def test_motor_fault_sets_relay_safe(self):
        """motor fault 시 릴레이가 HIGH(모터 전원 OFF)로 설정된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        assert sm.relay_high is False  # activate 중 = LOW

        sm._motor_open_timeout = True
        sm.handle_runtime_recovery()
        assert sm.relay_high is True   # safe stop → HIGH

    def test_motor_fault_disables_game_timer(self):
        """motor fault 시 GameTimer가 비활성화된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        assert sm._timer_enabled is True

        sm._motor_open_timeout = True
        sm.handle_runtime_recovery()
        assert sm._timer_enabled is False

    def test_motor_fault_does_not_call_uart_recovery(self):
        """motor fault는 Beetle UART 재초기화를 실행하지 않는다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm._motor_open_timeout = True
        sm.handle_runtime_recovery()

        assert sm.beetle_recover_attempts == 0

    def test_motor_fault_reason_recorded(self):
        """fault 원인 메시지가 systemFaultReason에 기록된다."""
        sm = EscapeMainSM()
        sm.data_changed(game_state="activate")
        sm._motor_close_timeout = True
        sm.handle_runtime_recovery()
        assert sm.system_fault_reason != ""
        assert "timeout" in sm.system_fault_reason.lower()
