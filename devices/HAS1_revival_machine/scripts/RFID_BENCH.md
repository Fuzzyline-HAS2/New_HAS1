# PN532 거리별 인식 진단

대상은 `HAS1_revival_machine`의 **RFID 벤치 빌드**입니다. 저장소의 빌드 규약과
의존성 버전을 사용합니다. 일반 운영 펌웨어에는 이 명령이 없으므로 `status` 응답이 없으면
멈춥니다. 예전 `MEASURE.md`의 서버 승인·릴레이 지연 측정과는 별개입니다.

## 준비

- 테스트 장치만 USB로 연결하고 다른 시리얼 모니터를 닫습니다. 한 프로그램만 포트를 소유합니다.
- 실제 태그, 비금속 고정대, 거리 표시를 준비합니다. 카드의 평면 방향과 중심 위치도 고정합니다.
- Python 3.9+를 사용합니다. USB 접근에만 `pyserial`이 필요하며 오프라인 분석·테스트에는 필요 없습니다.
  설치가 필요하다면 프로젝트용 가상환경을 사용하고 전역 환경은 바꾸지 않습니다.
- 결과는 Git에서 제외되는 `build/rfid-bench/`에 보관합니다. 측정 전 운영 릴레이·서버 요청이
  실행되지 않는 진단 펌웨어인지 부팅 로그와 `status`로 확인합니다.

```sh
mkdir -p build/rfid-bench
python3 devices/HAS1_revival_machine/scripts/rfid_bench.py ports
```

포트 열기는 한 번만 수행하며 DTR/RTS를 미리 해제합니다. 다만 USB 드라이버에 따라 여는 순간
리셋될 수 있습니다. 초기 `ready`를 받으면 같은 연결에서 `status`를 다시 요청합니다.
핸드셰이크 후 재부팅이 감지되면 기록을 남기고 중단하며, 스캔을 자동 재실행하지 않습니다.
ESP 부팅이나 PN532 초기화가 오래 걸려 타임아웃이 나면 원본 캡처를 확인하고 부팅 완료 후
새 파일 이름으로 다시 시작합니다.

## 진단 빌드 활성화와 복원

저장소 루트에서 [빌드 타깃 매핑](../../../scripts/firmware_targets.py)의 Revival FQBN과
[저장소 README](../../../README.md), [라이브러리 규약](../../../libraries/README.md)을 따릅니다.
ESP32 코어와 라이브러리 버전은 [배포 워크플로](../../../.github/workflows/deploy-firmware.yml)의
설정을 사용합니다. New_HAS1의 `HAS2_Wifi`는 `first_store`이며, 전역 스케치북에 다른 매장
라이브러리가 섞이지 않았는지 확인합니다. 별도 라이브러리 디렉터리를 쓰는 환경이라면 기존
빌드 명령의 `--libraries` 등 경로 설정도 그대로 유지합니다.

진단 기능은 기본값 `REVIVAL_RFID_DIAGNOSTICS=0`으로 꺼져 있습니다. 준비된 기존 빌드 환경에
다음 플래그를 추가하고 운영 산출물과 다른 디렉터리에 컴파일합니다.

```sh
REVIVAL_BENCH_FQBN="$(python3 -c 'from scripts.firmware_targets import TARGETS; print(TARGETS["HAS1_revival_machine"][1])')"
arduino-cli compile \
  --fqbn "$REVIVAL_BENCH_FQBN" \
  --build-property compiler.cpp.extra_flags=-DREVIVAL_RFID_DIAGNOSTICS=1 \
  --build-path build/rfid-bench/compile \
  --output-dir build/rfid-bench/firmware \
  devices/HAS1_revival_machine
```

이 작업은 로컬 진단용이며 버전·파티션 버전을 올리거나 릴리스를 만들지 않습니다. 운영용
컴파일에서는 진단 플래그를 빼거나 `=0`을 지정하고, 진단 빌드와 빌드/출력 디렉터리를 분리합니다.

**Academy 장치(예: AR)를 테스트할 때는 핀 배치 호환 여부를 먼저 확인하고, 업로드 전 원래
Academy 펌웨어와 플래시 상태를 백업합니다. 테스트 후에는 검증된 원래 Academy 백업을
복원합니다.** 이 저장소에서 진단 플래그를 끈 `first_store` 운영 펌웨어를 Academy 장치의
복원용으로 업로드하면 안 됩니다. 빌드 성공은 매장 설정 호환성을 보장하지 않습니다.

## 한 조건에서 측정

카드를 지정 위치에 **정지시킨 다음** 명령을 실행합니다. 아래 포트 이름은 예시이며 `ports` 결과를
사용합니다. 기존 출력 파일은 덮어쓰지 않습니다.

```sh
python3 devices/HAS1_revival_machine/scripts/rfid_bench.py scan \
  --port /dev/cu.usbserial-EXAMPLE \
  --gain 18 --timeout-ms 250 --retries 10 \
  --trials 20 --interval-ms 300 --deadline-s 120 \
  --card glove-A --reader revival-A --distance-mm 10 --orientation parallel \
  --note '케이스 장착, 카드와 안테나 중심 정렬' \
  --out build/rfid-bench/glove-A_10mm_gain18.jsonl
```

`--gain`은 `18`, `23`, `33`, `38`, `auto` 중 하나입니다. 한 번에 거리나 감도 등 한 조건만
바꾸고, 문제 위치를 포함해 같은 카드·방향으로 비교합니다. 20회는 **계속 놓인 카드에 대한
20회 연속 폴링**이며 20번 독립적으로 카드를 가져다 댄 실험이 아닙니다. 독립 태깅 성공률을
보려면 각 시행의 제거·접근·정지 조건과 충분한 RF/카드 상태 초기화를 별도로 통제해야 합니다.
카드 없는 초기 연결 검사에서는 `--card none --orientation unknown`으로 기록하고
`--distance-mm`를 생략합니다. 알 수 없는 거리를 임의로 입력하지 않습니다.

고정 감도 모드는 매 시도 후 RF를 재설정하지 않습니다. `auto`는 현재 운영 코드와 같은 감도
순환과 전부 실패한 뒤의 RF off/on을 사용합니다. 따라서 **고정 모드와 auto의 차이는 감도만의
차이가 아닙니다.** 먼저 고정 모드끼리 비교하고 auto는 실제 순환 경로 비교용으로 사용합니다.
`--reset`은 요청한 경우에만 PN532를 재초기화하며 매 시도 카드 전원 재설정을 뜻하지 않습니다.

스캔은 지정 횟수 후 종료됩니다. `Ctrl-C` 또는 시간 제한 도달 시 원본 파일을 보존하고
`stop`을 보내 최대 3초 동안 응답을 기다립니다. 펌웨어는 실행 중인 한 시도가 끝난 뒤 정지하므로
정지 응답이 확인되지 않으면 `stop_unconfirmed`가 기록됩니다. 장치의 스캔 자체도 유한 횟수입니다.

단계 로그는 각 시행 동안 메모리에 모았다가 **해당 시행 직후** 출력합니다. 전체 배치가 끝날
때까지 모으는 방식은 아닙니다. 다음 시행 시작까지는 직전 시행의 측정 시간 외에도 로그 출력
시간과 `--interval-ms`로 지정한 최소 간격, 루프 처리 시간이 들어갑니다. 따라서 시행 시작
간격을 RFID 읽기 소요시간으로 사용하지 않습니다.

## 기록과 분석

```sh
python3 devices/HAS1_revival_machine/scripts/rfid_bench.py analyze \
  build/rfid-bench/glove-A_10mm_gain18.jsonl

python3 devices/HAS1_revival_machine/scripts/test_rfid_bench.py
```

JSONL에는 조건 이름·로컬 Git SHA/변경 여부, 명령, MCU 원문과 바이트(hex), 해석한 진단 JSON,
호스트 수신 시각(UTC와 monotonic)이 들어갑니다. Git SHA는 **캡처 도구가 실행된 체크아웃**이며
보드에 업로드된 바이너리의 SHA를 증명하지 않습니다. 펌웨어 버전·빌드 로그도 함께 보관합니다.
분석은 다음을 구분합니다.

| 결과 필드 | 의미 |
|---|---|
| `requested` / `observed` / `missing` | 요청 횟수 / 캡처한 고유 시행 수 / 유실 또는 미실행 횟수 |
| `uid_failures` | UID 인식에 실패한 시행 |
| `payload_failures_after_uid` | UID를 읽었지만 page 7 데이터를 읽지 못한 시행 |
| `invalid_payloads` | page 7은 읽었지만 GxPx 형식이 아닌 시행 (`MMMM`도 여기에 포함) |
| `*_success_rate_of_requested` | 누락·실패 시행을 제외하지 않은 전체 요청 대비 성공 비율 |
| `*_success_scan_latency_ms` | 성공 시행의 MCU 스캔 소요시간 n·p50·p95·최댓값 |
| `failed_scan_latency_ms` | 실패 시행의 MCU 소요시간 통계 |
| `failed_stages_including_recovered_trials` | 최종 성공 전 재시도 실패도 포함한 단계별 실패 수 |

소요시간은 **MCU가 스캔을 시작해서 끝낼 때까지**입니다. 카드를 실제로 가져다 댄 시각부터
인식한 시간도, 서버 승인이나 릴레이 시간도 아닙니다. 호스트 수신 시각에는 USB 버퍼·출력 지연이
포함되고 단계별 로그는 시행이 끝난 뒤 출력되므로 이를 물리적 접근 시각으로 해석하면 안 됩니다.
접근·이탈 지연을 측정하려면 별도 위치 센서 또는 시각이 명확한 외부 마커가 필요합니다.

`gain` 단계의 성공은 PN532 명령 응답 경로의 성공이며 실제 아날로그 레지스터를 읽어 검증한
값이 아닙니다(`hardware_gain_confirmed:false`). `software_gain_db`도 소프트웨어 설정값입니다.
판독 성공/실패는 물리적인 RF 조건에 대한 별도 증거이며 호스트 단위 테스트가 이를 재현하지 않습니다.

## 시리얼 프로토콜

115200 baud, 줄바꿈 LF를 사용합니다. 모든 진단 레코드는
`[RFID_DIAG] {"protocol":1,"event":"...",...}`입니다.

| 명령 | 응답/동작 |
|---|---|
| `status` / `help` | 현재 설정과 지원 명령 |
| `gain auto\|18\|23\|33\|38` | 감도 설정 후 `ack` |
| `timeout 1..250` | UID 감지 타임아웃(ms) 후 `ack` |
| `retries 0..50` | PN532 활성화 재시도 횟수 후 `ack` |
| `scan 1..1000 [0..10000]` | 횟수와 시행 사이 간격(ms); `ack` → `trial` 반복 → `done` |
| `stop` | 현재 시행 완료 후 정지 |
| `reset` | 대기 중 PN532 재초기화 |

스캔 중 설정 변경은 거절됩니다. `error`, `ack`의 `ok:false` 또는 `ok` 누락은 실패로 취급합니다.
`trial.ok`는 page 7 읽기 성공, `uid_ok`는 UID 인식 성공, `valid_gxpx`는 게임 태그 형식 검증
결과입니다. 캡처 실패 시에도 파일을 지우지 말고 `analyze`로 누락 수와 중단 위치를 확인합니다.

## 운영 경로를 유지하는 런타임 추적

서버·BLE·LED가 함께 동작할 때의 지연은 `REVIVAL_RFID_RUNTIME_TRACE=1`로 별도 측정합니다.
기본값은 `0`이며, 벤치 진단과 동시에 켜면 컴파일 오류가 납니다. 이 모드는 정상
`setup()`/`loop()`와 게임·관리자 카드·서버 승인·릴레이 동작을 그대로 실행합니다.
벤치의 `status`/`scan` 명령은 제공하지 않으므로 `rfid_bench.py scan`으로 제어하지 않습니다.

기존 빌드 환경의 FQBN/라이브러리 설정을 유지하고 다음 플래그와 별도 출력 경로를 사용합니다.

```sh
arduino-cli compile \
  --fqbn "$REVIVAL_BENCH_FQBN" \
  --build-property 'compiler.cpp.extra_flags=-DREVIVAL_RFID_DIAGNOSTICS=0 -DREVIVAL_RFID_RUNTIME_TRACE=1' \
  --build-path build/rfid-bench/runtime-compile \
  --output-dir build/rfid-bench/runtime-firmware \
  devices/HAS1_revival_machine
```

USB 115200 baud에서 `[RFID_TRACE] ` 접두사의 JSONL을 일반 로그와 함께 수집합니다.
초기화 완료 후 `startup`에 `runtime_trace:true`, `diagnostics:false`, `fw:67`,
게임/기기 상태, UID 250ms 및 RF 설정 명령 100ms 상한을 표시합니다. page 7 판독도 100ms 상한이며 ACK·응답이 같은 예산을 공유합니다. 추적 레코드는 Telnet으로 복제하지
않고 `HardwareDebugSerial`로만 출력합니다. 운영 로그의 기존 Serial/Telnet 출력은 유지됩니다.

- `loop`는 실제 스캔이 있었거나 루프 작업이 40ms 이상 걸렸을 때만 출력합니다.
  디바운스로 건너뛴 폴링을 스캔 시행으로 세지 않습니다.
- `timer_us`, `neo_us`, `telnet_us`, `rfid_game_us`는 해당 운영 루프 구간의 MCU 측정값입니다.
  `rfid_game_us`에는 PN532 판독 뒤 역할 조회·승인 요청·릴레이 펄스도 포함될 수 있습니다.
- `scan.ctx`는 `gameplay`, `admin_pending`, `admin_ready`이며 `start_us`/`us`는 실제
  PN532 스캔 시작/소요시간입니다. `gap_us`는 **직전 실제 스캔 종료 → 이번 시작**이고,
  첫 스캔은 `null`입니다. 승인 대기 중 관리자 전용 스캔도 누락하지 않습니다.
- `post_read_us`는 스캔 종료부터 루프 작업 종료까지입니다. 스캔이 없으면 `scan`과
  `post_read_us`는 `null`입니다. `game_state`/`device_state`는 루프 종료 시 상태입니다.
- `stages` 각 항목은 `[단계, 요청 gain 바이트, 스캔 기준 시작 µs, 소요 µs, 성공 1/0, page7 앞4바이트 hex]`입니다.
  단계는 `uid`, `page7`, `gain`, `rf_off`, `rf_on`입니다. gain 값 `9/25/73`은 기존
  `0x09/0x19/0x49` 요청값이며 하드웨어 레지스터 읽기나 dB 검증 결과가 아닙니다.
  `gain`/`rf_off`/`rf_on`의 성공은 ACK와 전체 응답 프레임 검증 완료를 뜻합니다.
  실패한 판독의 payload는 빈 문자열입니다. `false` 자체만으로 RF 문제나 타임아웃을
  확정할 수 없으며, 단계별 시간과 PN532 라이브러리 반환 의미를 함께 확인해야 합니다.
- `prev_emit_us`/`prev_emit_bytes`는 직전 추적 레코드의 포맷·USB 쓰기 호출 비용/바이트입니다.
  현재 `loop_us`는 현재 추적 출력 시간을 제외하지만 다음 `gap_us`에는 그 지연이 포함됩니다.
  UART 버퍼에 남은 바이트의 실제 전송 완료 시간까지 측정하는 값은 아닙니다.

추적은 고정 크기 저장소만 사용하고 PN532 트랜잭션 사이에는 출력하지 않습니다.
현재 루프당 실제 스캔 최대 1건, 단계 최대 16개를 저장하며, 초과 스캔/포맷 실패는 누적
`dropped`, 초과 단계는 해당 루프의 `stage_dropped`로 표시합니다. 출력 버퍼는 1536바이트이고
일반 레코드는 약 1KB 미만입니다. 115200 baud 출력 자체가 다음 스캔을 늦출 수 있으므로
추적을 끈 빌드와도 비교해야 합니다. `micros()`는 약 71분마다 순환하며 구간 차이는 unsigned
연산으로 계산합니다. 한 구간이 순환 주기 이상이면 시간값이 모호해집니다.

성공한 스캔 기록은 5초 릴레이 펄스가 끝난 뒤 출력될 수 있습니다. PC 수신 시각을 판독 시각으로
해석하지 말고 MCU `start_us`와 구간 시간을 사용하고, 물리적 태그 접근·제거 시각은 별도로 기록합니다.
복원 시 두 플래그를 모두 `0`으로 컴파일합니다. 호스트 테스트는 다음과 같습니다.

```sh
python3 devices/HAS1_revival_machine/tests/run_rfid_runtime_trace_tests.py
python3 devices/HAS1_revival_machine/tests/run_rfid_diagnostic_tests.py
python3 devices/HAS1_revival_machine/tests/run_host_tests.py
```

복구 후보 빌드의 `status`는 `recovery_required`, `recovery_locked`,
`recovery_attempts`, `gain_known`, `last_outcome`, `transport_fault`,
`transport_phase`, `spi_status`도 표시합니다. 전송 장애 중에는 `pn532_ready`를
false로 보고하고 새 `scan`/gain 설정을 거절합니다. 진단 모드의 `reset`만
명시적으로 제한된 재초기화를 시도하며, 실제 PN532 하드웨어 리셋 핀은 연결되어 있지 않습니다.
