# HAS1_tagmachine_main

TTGO ESP32 기반 태그머신 메인 코드입니다.

## 주요 기능

- 메인/서브 Beetle 시리얼 태그 입력 처리
- WiFi 서버 상태 동기화
- 릴레이 기반 도어 제어
- DFPlayer 효과음 재생
- 네오픽셀 상태/게이지 연출
- 일반 모드 및 뉴비모드 처리
- Main/Sub Beetle과 TTGO의 순차 HMAC 검증 OTA

## 순차 OTA

운영 명령은 정확한 버전을 고정하는 형식을 권장합니다.

| `device_state` | 동작 |
| --- | --- |
| `github@14:4` | TTGO v14, Main/Sub Beetle 공통 v4를 순서대로 확인·설치 |
| `github-ttgo@14` | Beetle을 건너뛰고 TTGO v14만 긴급 확인·설치 |
| `github` | 각 고정 Release의 서명된 최신 포인터를 한 번 읽어 위 전체 순서 실행 |
| `github-ttgo` | 서명된 최신 포인터로 TTGO만 실행 |

`github@<TTGO>:<Beetle>`은 서버 명령 자체가 목표 버전이므로 재생된 예전 최신
포인터의 영향을 받지 않습니다. `github` 호환 명령도 포인터와 이미지 HMAC을 모두
검증하고 다운그레이드를 거부하지만, 포인터는 가변 채널이므로 캐시나 재생 공격에
의해 새 릴리스를 늦게 볼 수 있습니다. 운영 배포에는 정확한 버전 명령을 사용합니다.

전체 순서는 다음과 같습니다.

1. 명령과 서명 manifest에서 TTGO/Beetle 목표를 한 번 고정합니다.
2. Main Beetle의 OTA 프로토콜 v2·현재 버전·파티션을 확인하고
   `U:<request>:<target>`을 보냅니다. Beetle이 Wi-Fi로 변경 불가한
   `HAS1_tagmachine_sub-v<target>` 아카이브를 검증·설치한 뒤, 새 boot ID와 정확한
   실행 버전 및 PN532 ready를 TTGO가 확인합니다.
3. 같은 Beetle 목표 버전으로 Sub Beetle을 확인합니다. 두 Beetle 사이에 새 Release가
   게시되어도 서로 다른 버전을 설치하지 않습니다.
4. 두 보드가 증명된 뒤에만 TTGO가 `HAS1_tagmachine_main-v<target>`을 설치합니다.
   설치 의도는 NVS에 먼저 기록하며 서버 명령은 지우지 않습니다. 새 TTGO 부팅에서
   정확한 버전·파티션을 확인한 뒤에만 명령을 `setting`으로 되돌립니다.

Beetle의 명시적 pre-commit 실패는 새 request ID로 최대 3회(5초/10초 간격) 다시
시도합니다. flash 결과가 불확실한 timeout은 자동 실패나 재쓰기 대신 릴레이를 잠근
채 같은 request의 재부팅 증명을 기다립니다. TTGO-only 경로도 실제 이미지 확인은
최대 3회만 수행하며 지수 backoff를 사용합니다. 확인이 끝난 뒤 게임이 활성화되면
재시도 요청만 유지하고 격리와 릴레이 잠금은 즉시 해제합니다.

모든 OTA 시작 직전 서버의 명령과 `setting`/`ready` 단계를 다시 읽습니다. OTA 중에는
게임/장치 콜백과 태그 처리를 억제하되 UART 상태/결과 수신은 계속합니다. 서버 명령이나
게임 단계가 바뀌면 현재 쓰기 중인 보드의 결과까지 확인한 뒤 다음 보드로 넘어가지
않습니다. 결과 프레임은 모두 `R`로 시작하므로 구형 TTGO도 태그로 오인하지 않습니다.

현장에 설치된 Beetle v3에는 OTA 코드가 없습니다. 최초 OTA 베이스라인(프로토콜 v2)은
Main/Sub Beetle 두 대에 USB로 한 번 설치해야 하며, v1 프로토콜 OTA 빌드가 이미 있다면
그 빌드도 target-bearing v2 명령을 이해하지 못하므로 USB로 교체합니다. 이후부터
원격 업데이트할 수 있습니다. 두 C3의 UART TX는 부트 스트랩 GPIO9를 사용하므로,
특히 Main Beetle은 OTA 후 정상 앱으로 재부팅되는지 현장에서 먼저 확인해야 합니다.

배포 워크플로는 TTGO(`HAS1_tagmachine_main`)와 Beetle 공용
(`HAS1_tagmachine_sub`) 각각의 고정 Release와 변경 불가 버전 Release를 게시하고,
`ota.txt`/`ota.sig`가 가리키는 버전·보드·파티션·이미지 HMAC을 검증합니다. 두
TagMachine 앱은 0x140000-byte OTA 슬롯에서 최소 32KiB 여유가 없으면 CI/배포를
중단합니다.

최초 롤아웃은 다음 순서를 지킵니다.

1. `HAS1_tagmachine_sub`를 먼저 릴리스하고 두 Beetle 모두에 프로토콜-v2 베이스라인을
   USB 설치합니다.
2. `HAS1_tagmachine_main`을 릴리스합니다. 기존 TTGO v13은 버전 지정 명령을 모르므로
   이 한 번은 기존 `github` 명령으로 TTGO v14를 설치합니다.
3. TTGO v14와 두 Beetle v4, 파티션 1, PN532 ready 및 GPIO9 재부팅을 확인합니다.
4. 이후 배포부터 `github@<TTGO>:<Beetle>`을 사용합니다.

현재 서버 API는 `device_state`의 compare-and-set을 제공하지 않습니다. 펌웨어는 쓰기
직전/직후 명령을 다시 읽어 새 명령을 가능한 한 보존하지만, read와 write 사이의 매우
짧은 경쟁을 완전히 없애려면 서버에 command generation 또는 조건부 갱신 API가
추가되어야 합니다.

## 문서

- `report.md`: 최근 수정 내용과 문제 해결 보고서
