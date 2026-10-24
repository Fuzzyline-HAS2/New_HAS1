# Blackbox Phase 1 검증 기록

2026-10-24 / 기준 `5f2ce69` / 브랜치 `codex/blackbox-mqtt`.
상태: 소프트웨어 구현·자동 시험·4종 펌웨어 빌드·독립 검토 완료. **실제 Windows/ESP32 현장 검증과 배포는 미실행.**

## 구현 범위

- 별도 HAS1BlackBox: raw flash 순환 기록, CRC/readback, 128-slot metadata journal, sequence 예약, 제한 RAM 큐, 별도 writer/uploader.
- 글러브 TTGO/training 및 발전기 관측 hook. 게임 HTTP/OTA/복구 정책은 유지.
- MQTT QoS1 → Windows Mosquitto → 독립 Python 수집기 → SQLite FULL COMMIT → application ACK.
- 수집기 실행/설정, JSONL export, 읽기 전용 raw partition decoder, Windows/Linux CI.
- broker URI 기본값은 빈 문자열: **기본 flash-only, 실제 MQTT 업로드는 현장 설정 후 활성화**.

순환 공간은 1,920칸이다. 정상 기록량 기준 최근 약2~3시간을 예상하지만 실측 보장값은 아니다.
미수집 기록도 가득 차면 오래된 섹터부터 덮어쓰고 가능한 한 LOST 범위를 남긴다.
몇 주 이력을 보관하려면 정상 접속 시 Windows로 지속 회수해야 한다. 최초 장애 영구 고정은 구현하지 않았다.

## 실행한 자동 검증

| 대상 | 결과 | 확인 내용 |
|---|---|---|
| HAS1BlackBox core | 통과 | ASan/UBSan, -Wall/-Wextra/-Werror, CRC/byte order/sequence/ACK, 65개 write 절단 경계, 부분 erase/read 실패, metadata/LOST 오류, 순환·재부팅·재전송 |
| 글러브 C++ host | 20개 실행기 통과 | 새 blackbox 정책, ADC 미측정과 실제0 구분, 기존 게임/UART/복구, LED guard 거부 후 재전송 |
| 글러브 Python 도구 | 26 tests 통과 | 기존 OTA 양 경로, pause/resume, 타장치 빌드 격리, source provenance |
| 발전기 host | 13개 실행기 통과 | 새 blackbox/LED 충돌, 기존 통신48사례, RTC 진단, boot, wire/timer/tagger, DFPlayer/HTTP/Telnet 회귀 |
| 수집기 | 31 tests 통과 (skip0) | 실제 Mosquitto2.0.22, 인증/제공 ACL, 다른 장치 ACK·배치 위조 거부, 브로커 재시작, COMMIT 전후 프로세스 종료, SQLITE_FULL, 충돌/중복/CRC/큐 포화, raw decoder |
| C++ ↔ Python 계약 | 통과 | 실제 C++ 16-record 인코더 → Python 파서 → 실제 SQLite → C++ ACK 수락/잘못된 nonce 거부. 실제 C++ Ring의 600회 checkpoint·재부팅 dump → Python decoder 검증 포함. 위31개에 포함 |
| 독립 코드 검토 | 통과 | 저장·ACK·동시성·OTA 경계 검토 및 발견 사항 수정 확인 |
| 변경 격리 | 통과 | 기존 공용 라이브러리/공용 patch/OTA dependency pin/partition target/Beetle·TagMachine 소스 및 버전 변경 없음 |

검토 중 수정: 글러브 업로드 허용 상태의 두 작업 간 경쟁 조건, 미측정 ADC=0 오인,
MQTT SDK enum 타입, 테스트 빌드의 문자열 인자 quoting, 순환 덮어쓰기 문서의 ACK 예외 설명,
메타데이터를 매번 erase하던 방식을 128-slot journal로 변경했다.

메타데이터 내구성 모의 시험에서 **4,161회 쓰기 / 64회 erase(섹터별32회)**를 확인했다.
기존 매회 erase 방식 대비 64배 이상 줄었으며, 실제 flash 수명 보장은 아니다.
섹터 전환 시 65개 write 절단 경계·7개 부분 erase, 손상 슬롯 재사용 방지, 기존 2-header 형식 복구,
128개 슬롯의 ring/generation 충돌 거부도 검증했다.

## 펌웨어 빌드

Arduino-ESP32 **3.3.11**, ESP-MQTT는 이 SDK에 포함된 header/static library 사용.
공개 테스트 HMAC 키로 기존 OTA 경로도 링크했다. 결과물은 **절대 flash/publish하지 않는 검증 전용**이다.
각 빌드의 `COMPILE_ONLY.txt`를 유지한다. 실기기에 기록하거나 릴리즈하지 않았다.

| 대상 | 프로그램 사용 bytes | 정적 전역 RAM bytes | 상태 |
|---|---:|---:|---|
| TTGO MQTT + OTA | 1,196,209 | 73,576 | 통과 |
| training MQTT 설정 | 647,557 | 55,156 | 통과 |
| 발전기 MQTT + OTA | 1,411,893 | 101,420 | 통과 |
| Beetle (새 logger 제외) | 1,407,715 | 43,224 | 통과 |

기존 `min_spiffs` OTA app slot은 1,966,080 bytes이다. 정적 RAM 수치에는 런타임 heap/task/SDK 할당이 전부 포함되지 않는다.
SDK `min_spiffs.csv`의 spiffs는 0x3D0000/0x20000이며, 실제 장치 파티션이 동일한지는 별도 확인해야 한다.
대상 소스/의존성에서 기존 SPIFFS/LittleFS mount 사용은 발견하지 못했다. 알 수 없는 비어 있지 않은 파티션은 자동 삭제하지 않는다.

## 재실행

```sh
python3 libraries/HAS1BlackBox/tests/run_tests.py
python3 devices/iotglove/tools/run_tests.py
python3 -m unittest discover -s devices/iotglove/tools -p 'test_*.py'
python3 devices/HAS1_generator/tests/run_blackbox_tests.py
python3 -m unittest discover -s tools/windows-blackbox-logger/tests -v
```

실제 MQTT 시험은 `paho-mqtt==2.1.0`와 Mosquitto가 필요하다. 없으면 해당 integration test가 skip되며 통과로 간주하지 않는다.
`MOSQUITTO_BIN`으로 broker 실행 파일을 지정할 수 있다. C++ 계약 시험에는 C++17 compiler와 저장소의 library source가 필요하다.
발전기 전체 host suite의 일부는 `ARDUINOJSON_INCLUDE`에 ArduinoJson7.4.3/src가 필요하다.

```sh
python3 devices/iotglove/tools/compile.py all --libraries-dir <prepared-glove-deps> \
  --check-ota-paths --blackbox-mode mqtt --output-dir <compile-only-dir>
python3 scripts/compile_blackbox_generator.py --libraries-dir <generator-deps> \
  --blackbox-mode mqtt --output-dir <compile-only-dir>
```

글러브 의존성은 기존 `prepare_libraries.py`로 준비한다. 발전기는 기존 repository HAS2_Wifi를 사용하며 글러브 patch 버전으로 대체하지 않는다.
새 발전기 compile CI가 pinned core/registry/SecureOTA/Timer 설정을 제공한다.

## 미실행 — 현장 적용 게이트

1. **Windows 실제 실행/CI**: workflow를 작성했지만 이 로컬 작업에서는 Windows runner를 실행하지 않았다. 설치/시작/중지/경로/방화벽/서비스 복구를 확인해야 한다.
2. **실제 ESP32**: 전원 차단, 이동/로밍, Wi-Fi/서버/브로커 단절, OTA 병행, heap/stack/flash erase 시간, 미등록 파티션을 확인해야 한다.
3. **지연 비교**: logger OFF/flash-only/MQTT 각각30분×3 및8시간 soak, 텔넷0세션. 초기 후보 기준은 추가 game poll p99≤20ms/max≤100ms, loop p99≤5ms/max≤50ms.
4. **전원 원인**: ADC 미설정은 unknown이다. 순간 접점/전압 강하는 소프트웨어 로그만으로 확정할 수 없으며 필요시 별도 전원 계측과 결합한다.

writer/uploader/MQTT priority는1로 게임 worker와 동일하다. 시작 시 partition scan은 LED guard를 잡고, OTA pause도 이미 진행 중인 flash 작업을 중단하지 못한다.
이러한 지연은 host test나 빌드로 증명되지 않는다. 장치 로그가 실제 생성/회수되는지 확인한 소수 기기부터 도입한다.
