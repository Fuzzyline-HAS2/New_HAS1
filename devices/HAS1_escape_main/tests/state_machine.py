"""
updated_escape_main 소프트웨어 로직의 Python 시뮬레이션.
Wifi.ino(DataChanged, SettingFunc, ActivateFunc, ReadyFunc)와
Game_system.ino(TagCount)를 미러링합니다.
error_recovery.ino(LatchSystemFault, ClearSystemFault, HandleRuntimeRecovery) 포함.
하드웨어 없이 순수 소프트웨어 단위 테스트에 사용합니다.
"""


class EscapeMainSM:
    def __init__(self, device_name: str = "updated_escape_main"):
        self.device_name = device_name
        self.game_state = ""
        self.device_state = ""
        self._cur_game = ""       # DataChanged()의 cur["game_state"]
        self._cur_device = ""     # DataChanged()의 cur["device_state"]
        self.tag_state = [False, False, False]
        self.tag_cnt = 0
        self.relay_high = True    # True=HIGH(OFF), False=LOW(ON)
        self._mode = "wait"       # "wait"=WaitFunc, "tagcount"=TagCount
        self._timer_enabled = False
        self._sent: list = []     # has2wifi.Send() 호출 기록
        self._events: list = []   # Serial.println() 등 동작 기록

        # ── Motor timeout simulation (stepper_Motor.ino watchdog 대응) ──────
        self._motor_close_timeout = False  # EscapeClose() 10초 초과 시 True
        self._motor_open_timeout = False   # EscapeOpen() 10초 초과 시 True

        # ── Beetle 통신 오류 카운터 (error_recovery.ino 대응) ───────────────
        self.invalid_cmd_count = 0          # 허용되지 않은 명령 수신 횟수
        self.packet_format_error_count = 0  # T 패킷 포맷 오류 누적
        self.tag_parse_error_count = 0      # 태그 파싱 실패 누적
        self.beetle_bad_event_streak = 0    # 연속 bad-event 사이클 수
        self.beetle_recover_attempts = 0    # UART 복구 시도 횟수

        # HandleRuntimeRecovery delta 추적 (static 변수 대응)
        self._prev_cmd = 0
        self._prev_fmt = 0
        self._prev_tag = 0

        # ── 하드웨어 fault 래치 ─────────────────────────────────────────────
        self.system_fault_latched = False
        self.system_fault_reason = ""

    # ── Public API ──────────────────────────────────────────────────────────

    def data_changed(self, game_state: str = None, device_state: str = None):
        """DataChanged() 시뮬레이션."""
        if game_state is not None:
            self.game_state = game_state
        if device_state is not None:
            self.device_state = device_state

        if self.game_state != self._cur_game:
            if self.game_state == "setting":
                self._setting_func()
            elif self.game_state == "ready":
                self._ready_func()
            elif self.game_state == "activate":
                self._activate_func()

        if self.device_state != self._cur_device:
            if self.device_state == "player_win":
                self._clear_system_fault()
                self._ev("AllNeoOn(BLUE)")
                self._ev("EscapeClose")

        self._cur_game = self.game_state
        self._cur_device = self.device_state

    def set_tags(self, t1: bool, t2: bool, t3: bool):
        """Beetle T패킷 수신 시뮬레이션."""
        self.tag_state = [t1, t2, t3]

    def tick_game_timer(self):
        """GameTimer tick 시뮬레이션 — ptrCurrentMode() 호출."""
        if self._mode == "tagcount":
            self._tag_count()

    def receive_bad_event(self, kind: str):
        """
        Beetle bad event 수신 시뮬레이션.
        kind: 'invalid_cmd' | 'format_error' | 'tag_parse'
        silence는 bad event가 아니므로 여기에 없음.
        """
        if kind == "invalid_cmd":
            self.invalid_cmd_count += 1
            self._ev("[UART] WARN unknown command")
        elif kind == "format_error":
            self.packet_format_error_count += 1
            self._ev("[UART] WARN malformed T packet")
        elif kind == "tag_parse":
            self.tag_parse_error_count += 1
            self._ev("[UART] WARN tag role unresolved")

    def handle_runtime_recovery(self):
        """
        HandleRuntimeRecovery() 시뮬레이션.
        timer.ino의 WifiIntervalFunc / GameTimerFunc에서 매 tick 호출됨.
        """
        # 1) 모터 timeout → 즉시 safe stop
        if self._motor_close_timeout or self._motor_open_timeout:
            reason = ("EscapeClose() timeout" if self._motor_close_timeout
                      else "EscapeOpen() timeout")
            self._motor_close_timeout = False
            self._motor_open_timeout = False
            self._latch_system_fault(reason)
            return

        # 2) bad event delta 감지 (silence는 무시)
        if self.invalid_cmd_count < self._prev_cmd:
            self._prev_cmd = self.invalid_cmd_count
        if self.packet_format_error_count < self._prev_fmt:
            self._prev_fmt = self.packet_format_error_count
        if self.tag_parse_error_count < self._prev_tag:
            self._prev_tag = self.tag_parse_error_count

        new_bad = (self.invalid_cmd_count > self._prev_cmd or
                   self.packet_format_error_count > self._prev_fmt or
                   self.tag_parse_error_count > self._prev_tag)

        self._prev_cmd = self.invalid_cmd_count
        self._prev_fmt = self.packet_format_error_count
        self._prev_tag = self.tag_parse_error_count

        if new_bad:
            self.beetle_bad_event_streak += 1
            self._ev(f"[RECOVERY] bad event streak={self.beetle_bad_event_streak}")

        if self.beetle_bad_event_streak >= 3:
            self._recover_beetle_connection()
            self._prev_cmd = 0
            self._prev_fmt = 0
            self._prev_tag = 0
            if self.beetle_recover_attempts >= 3:
                self._ev("[RECOVERY] Max attempts, restart")

    def reset_beetle_error_counters(self):
        """ResetBeetleErrorCounters() 시뮬레이션."""
        self.invalid_cmd_count = 0
        self.packet_format_error_count = 0
        self.tag_parse_error_count = 0
        self.beetle_bad_event_streak = 0

    # ── Private (Arduino 함수 대응) ─────────────────────────────────────────

    def _setting_func(self):
        """Wifi.ino SettingFunc()"""
        self._clear_system_fault()
        self._ev("SETTING")
        self.relay_high = True
        self._ev("AllNeoOn(WHITE)")
        self._ev("EscapeClose")
        if self._motor_close_timeout:
            return
        self._mode = "wait"
        self._timer_enabled = False

    def _ready_func(self):
        """Wifi.ino ReadyFunc()"""
        self._clear_system_fault()
        self._ev("READY")
        self.relay_high = True
        self._ev("AllNeoOn(RED)")
        self._ev("EscapeClose")
        if self._motor_close_timeout:
            return
        self._mode = "wait"

    def _activate_func(self):
        """Wifi.ino ActivateFunc()"""
        self._clear_system_fault()
        self._ev("ACTIVATE")
        self._ev("MP3 VE1")
        self._ev("AllNeoOn(YELLOW)")
        self._ev("EscapeOpen")
        if self._motor_open_timeout:
            return
        self.relay_high = False
        self._mode = "tagcount"
        self._timer_enabled = True

    def _tag_count(self):
        """Game_system.ino TagCount()"""
        self.tag_cnt = sum(1 for t in self.tag_state if t)

        if self.tag_cnt == 1:
            self._ev("Tag 1 detected")
            self._ev("MP3 VE2")
        elif self.tag_cnt == 2:
            self._ev("Tag 2 detected")
            self._ev("MP3 VE3")
        elif self.tag_cnt >= 3:
            self._ev("Escape Activate")
            self._ev("MP3 VE4")
            self._sent.append({
                "key": self.device_name,
                "column": "device_state",
                "value": "escape",
            })
            self._ev("Send device_state=escape")
            self._ev("EscapeClose")
            self._mode = "wait"
            self._timer_enabled = False
            self._ev("MP3 VE5")

        self.tag_state = [False, False, False]

    def _latch_system_fault(self, reason: str):
        """LatchSystemFault() 시뮬레이션."""
        if self.system_fault_latched:
            return
        self.system_fault_latched = True
        self.system_fault_reason = reason
        self.relay_high = True
        self._timer_enabled = False
        self._mode = "wait"
        self._ev(f"[SAFE] FAULT LATCHED: {reason}")

    def _clear_system_fault(self):
        """ClearSystemFault() 시뮬레이션. 전이 시작 직전 호출."""
        if not self.system_fault_latched:
            return
        self.system_fault_latched = False
        self.system_fault_reason = ""
        self._ev("[SAFE] Fault cleared. Ready for retry.")

    def _recover_beetle_connection(self):
        """RecoverBeetleConnection() 시뮬레이션."""
        self.beetle_recover_attempts += 1
        self._ev(f"[RECOVERY] UART reset #{self.beetle_recover_attempts}")
        self.reset_beetle_error_counters()

    # ── Assertion helpers ────────────────────────────────────────────────────

    def _ev(self, msg: str):
        self._events.append(msg)

    def get_events(self) -> list:
        return list(self._events)

    def clear_events(self):
        self._events.clear()

    def get_sent(self) -> list:
        return list(self._sent)

    def clear_sent(self):
        self._sent.clear()
