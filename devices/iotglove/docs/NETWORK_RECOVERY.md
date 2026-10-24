# 통신 복구 동작

2026-10-22 소스 변경. 이동 중 간헐적 단일 장치 오프라인의 현장 원인은 아직 확정되지 않았다. 확인된 HTTP 정체 경로와 복구 공백을 보완하며, 실기 검증은 별도다. 사용자 요청에 따라 OTA 구조와 공통 OTA 라이브러리는 기존 상태를 유지한다.

## 일반 HTTP

TTGO worker만 `EnableBoundedHttp()`를 활성화한다. 기존 checked bool API와 다른 장치의 기본 동작을 유지한다. 실제 빌드 의존성은 원격 HAS2_Wifi first_store에 저장소 패치 및 `tools/bounded_http`를 적용한 복사본이다. 전역 Arduino SDK는 수정하지 않는다.

Arduino-ESP32 3.3.11 HTTPClient의 응답 파서 메서드를 프로젝트 전용 subclass로 제한 수정했다. 연결·전송·헤더·본문·chunk/trailer에 요청 시작부터 5초, 실제 송수신 무진행 1.5초 제한을 적용한다. 현재 백엔드의 숫자 IPv4 HTTP만 지원한다. DNS, HTTPS, redirect 지원을 이 경로에 추가하려면 별도 시간 제한 검증이 필요하다.

본문은 읽는 도중 8KiB, 헤더 합계 4KiB/한 줄 1KiB, chunk 크기 줄 128byte, trailer 합계 2KiB, 전체 수신 16KiB로 제한한다. 길이·chunk 종료가 완전하지 않은 200 응답은 JSON에 반영하지 않는다. 정상 헤더가 끝난 비-200 응답은 `HttpStatus`로 분류하고 소켓을 닫는다. 불필요한 오류 본문 수신은 생략하므로 서버 오류 본문 정체가 Wi-Fi 재접속을 유발하지 않는다. SDK `end()`의 잔여 수신 버퍼 비우기 전에 소켓을 닫아 종료 자체의 무한 대기도 방지한다.

오류 값은 `None=0`, `Transport=1`, `HttpStatus=2`, `Framing=3`, `TooLarge=4`, `Json=5`다. 유효한 HTTP/JSON을 받았어도 게임 상태 검증에 실패하면 snapshot은 invalid로 유지된다. `s` 진단의 server valid/fresh와 HTTP 결과를 함께 확인한다.

## 재연결과 명령 처리

| 조건 | 처리 |
| --- | --- |
| AP 연결 단절 | fixed `badland_shoot` 접속(시도당 10초), 실패 후 5초 대기. 접속해 있던 부팅에서 30초, 한 번도 못 붙은 부팅에서 180초 연속 미접속이면 작업 감시와 같은 재시작 예산으로 TTGO 재시작 |
| 주 조회의 전송 실패 3회 이상이며 첫 실패부터 15초 이상 | snapshot/session 무효화, STA 실제 종료, fixed AP 재접속 |
| 반복 강제 재접속 | 30→60→120초 간격 + 0~5초 jitter; 정상 snapshot 60초 연속 수신 후 초기화 |
| 주 조회의 HTTP 오류·framing·JSON/게임 상태 오류 | snapshot 무효화/재조회; 이 오류만으로 Wi-Fi 강제 초기화하지 않음 |
| HTTP 429/503 | 모든 서버 요청에 Retry-After 적용; 숫자 초만 지원하고 최대 120초, 없거나 날짜 형식이면 5초 |
| 연결 무효화 시 대기 게임 명령 | `Unknown` 결과로 취소하여 inFlight/busy 해제; 자동 재전송 없음 |
| 복구 후 | 새 snapshot/session을 확인하고 최신 물리 칩 절대값을 다시 동기화 |

실패 회수는 주기적 `ReceiveMine` 주 조회만 집계한다. 부수 요청 성공이 주 조회 실패를 가리거나, 한 회차의 여러 실패가 재접속 임계값을 부풀리지 않는다. 명령 전후 조회 실패도 게임 변경 요청의 자동 재전송 근거로 사용하지 않는다.

## 작업 감시와 OTA

worker가 단계·sequence·시작/진행/완료 시각을 짧은 critical section으로 공개한다. 메인은 복사 이후의 `millis()`와 비교하여 두 코어의 시각 역전으로 인한 오판을 피한다. 서버 성공 시각으로 watchdog을 판단하지 않는다.

- 일반 HTTP 작업 20초, 연결/시작 30초, idle 20초, 의도된 대기는 지정 대기+20초가 최후 감시 기한이다. 정상 실패/재시도 중인 서버 장기 중단은 재부팅 사유가 아니다.
- main loop 완료 지점을 [ESP-IDF task watchdog user](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/api-reference/system/wdts.html)로 감시한다. TTGO 본게임에서 기존 idle 구성을 보존하고 60초로 설정한다. 일반 HTTP는 더 짧은 별도 감시를 받는다.
- 정체가 기한을 초과하면 소켓 사용 중인 task를 강제 삭제하지 않고 TTGO 전체 재시작을 최후 수단으로 사용한다. RTC RAM에 복구 재시작 횟수를 기록하여 연속 3회 후 자동 재시작과 새 main watchdog 등록을 억제한다. 이 진단 모드에서도 USB/로컬 루프를 유지하고, worker가 10분간 계속 진행하면 예산을 회복한다. 독립적인 전원/하드웨어 watchdog까지 무력화하는 기능은 아니다.
- TTGO 최신 OTA는 기존 SecureOTA와 고정 태그의 `version.txt`, `update.bin`, `update.sig`를 사용한다. 최신 업데이트에 `ota.txt/ota.sig`나 버전별 archive를 새로 요구하지 않는다. 기존 skip/success 처리와 Beetle → TTGO 순서를 유지한다.
- 명시적으로 버전을 지정한 OTA는 원래의 `IoTGloveOtaClient::checkPinned`를 그대로 사용한다. 지정 버전에서 원래 요구하던 서명 메타데이터·archive, 파티션 검증·rollback 정책은 변경하지 않는다. 공통 OTA 클라이언트에 추가했던 transport/progress hook과 새 기한은 제거했다.
- 진행 hook이 없는 기존 OTA를 일반 HTTP 정체로 오판하지 않도록, **OTA 실행 및 완료 확인 콜백 중에는 새 worker 정체 감시를 적용하지 않는다.** skip/실패로 반환하면 일반 통신 감시를 재개한다. 메인 루프 자체의 감시와 기존 OTA 코드 내부 처리는 유지한다. 따라서 이번 통신 복구 변경은 OTA 내부의 무한 정체까지 새로 해결한다고 주장하지 않는다.

## 링크 손실 기록

2026-10-23 1호점 실측: 글러브 3대가 게임 중 Wi-Fi 링크를 잃은 뒤 핑·Telnet·HTTP가 함께 끊겼고, 그중 한 대는 전원이 켜진 채 27분간 AP 옆에서도 재접속하지 못하다가 배터리 재장착 1초 만에 붙었다. 접속 실패만으로는 재시작하지 않던 공백과, 끊기는 순간의 이유를 어디에도 남기지 못하던 공백을 메운다.

- worker는 확립된 링크의 **첫** STA 단절 이벤트만 손실로 센다. 이유 코드, 직전 RSSI, 발생 uptime을 보관하고 이후 재접속 시도가 만드는 단절 이벤트는 덮어쓰지 않는다. 강제 재접속의 의도적 단절은 손실이 아니다.
- 메인 루프는 새 손실을 1초 이내에 감지해 RTC RAM `LinkLossRecord`에 마지막 유효 서버 상태(phase/role/device_state), 모터 ON 여부와 마지막 ON 이후 경과, 점등 칸, heap, 배터리 raw ADC mV·보정 전압을 담는다. 재시작을 요청하면 원인(`wifi_down`/`worker_stall`), 요청 uptime, 접속 실패 횟수, 예산 차수를 채운다. 링크가 재시작 없이 돌아오면 기록을 지운다.
- RTC RAM은 소프트웨어·워치독·brownout 리셋을 견디므로 다음 부팅의 `[recovery] retained link loss:` / `[recovery] retained context:` 두 줄이 Telnet 수집기에 도달한다. 배터리를 빼면 지워진다. 운영자는 응답 없음 뒤 최소 70초를 기다려야 이 기록과 TASK_WDT 리셋 사유를 모두 얻는다.
- 배터리 핀 raw ADC는 보정 상수 없이도 1초마다 읽어 `s` 상태 `[battery] adc_mv=`와 손실 기록에 남긴다. 분압이 연결되지 않은 핀은 0 근처를 읽는다.
- `s` 상태의 `[link]` 줄: `up`, `was_up`, `down_ms`, `connect_failures`, `losses`, `last_loss_reason/rssi/age_ms`, `restart_after_ms`. worker는 손실·복구 시 `[net] link lost ...`, `[net] link restored after ... ms` 를 남긴다.

## 진단과 실기 합격 기준

USB 또는 Telnet `s`에 Wi-Fi/RSSI/단절 사유, worker 단계·기한, HTTP 결과/시간/바이트, 주 조회 실패/재접속 횟수, heap/최대 연속 공간/worker stack 최저 여유, reset reason/재시작 예산을 출력한다. 오류 본문이나 자격증명은 기록하지 않는다.

실기 한 대에서 AP 꺼짐/복귀, 서버 중단/복귀, 본문 일부 후 정지, TCP 정상 종료, 지속 slow drip, 실제 이동, OTA 정상/중간 단절을 분리해 시험한다. 합격 기준은 자동 재동기화, 무한 HTTP 대기 없음, 서버 장애만으로 재부팅 반복 없음, 중복 게임 변경 없음, 정상 OTA에서 감시 오작동 없음이다. 각 시험에서 firmware/core/라이브러리 manifest와 재접속/재부팅 횟수 및 복구 시간을 기록한다. 1Mbps 고정·AP/DHCP·서버 게임 규칙은 이번 변경 범위 밖이다.
