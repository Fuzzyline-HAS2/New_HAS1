# 1호점 로그 시간 비교

이 도구는 글러브·발전기, Omada, 게임 서버 로그를 **1호점(`store-1`) 식별자로
구분하여 UTC로 정리**합니다. 같은 시간대에 보이는 이벤트를 조사 후보로 모으지만,
시간 변환만으로 시계 오차를 없애거나 장애 원인을 확정하지는 않습니다.

현재 구성에서 Windows 수집기와 게임 서버는 같은 PC `172.30.1.43`에서 실행합니다.
둘 다 Windows 시스템 시계를 사용하는 경우 시계 기준을 공유하므로 비교가 쉬워집니다.
서버의 요청 발생 시각과 수집기의 파일 읽기 시각은 여전히 다른 값입니다. 같은 PC라도
별도 VM/컨테이너의 시계 사용 여부와 Windows 시간 동기화 상태를 확인해야 합니다.
Omada와 장치 시계까지 자동으로 동기화되는 것은 아닙니다.

## 저장 구조와 수집 범위

| 파일 | 용도 |
|---|---|
| `data/blackbox.sqlite3` | 기존 장치 원본 및 장치에 보내는 저장 완료 ACK의 근거 |
| `data/telemetry.sqlite3` | Omada·게임 서버·가져온 장치 로그 및 명시적인 시계 측정 자료 |

텔레메트리 수집은 별도 작업에서 실행합니다. 느린 Omada/서버 로그 처리가 장치 MQTT
ACK 작업에 들어가지 않습니다. 같은 디스크/CPU를 사용하므로 물리적인 자원 경쟁까지
없어지는 것은 아닙니다. 두 DB의 경로를 같게 지정할 수 없습니다.

Omada와 게임 서버의 자동 수집 설정은 [Omada 안내](OMADA.md)와
`config.store1.example.json`을 참고하십시오. 두 기능은 기본적으로 꺼져 있습니다.
게임 서버에는 이 형식으로 JSONL을 기록하는 서버 변경의 배포도 필요합니다.
서버 로그 디렉터리를 지정한다고 기존 일반 로그가 자동 해석되지는 않습니다.

장치 로그는 아래 `import-device` 명령으로 가져옵니다. 이 명령은 기존 장치 DB를
읽기 전용으로 열고 원본 바이트와 첫 수신 시간을 보존합니다. **장치 DB에 사이트
필드가 없으므로, 운영자가 이 DB가 1호점 전용이라는 점을 확인해야 합니다.** 여러
매장에서 섞어 사용한 DB를 통째로 `store-1`로 가져오지 마십시오.

## Windows 명령 예시

서비스 설치 위치의 전용 Python을 사용합니다. 관리자 명령 프롬프트에서 실행하면
ProgramData 설정/DB 권한 문제를 피할 수 있습니다. 서비스 외 콘솔 설치라면 실제
프로젝트 디렉터리와 `.venv\Scripts\python.exe` 경로로 바꾸십시오.

```bat
cd /d "%ProgramFiles%\HAS1 BlackBox"

.venv\Scripts\python.exe timeline.py --database "%ProgramData%\HAS1BlackBox\data\telemetry.sqlite3" import-device "%ProgramData%\HAS1BlackBox\data\blackbox.sqlite3" --site-id store-1

.venv\Scripts\python.exe timeline.py --database "%ProgramData%\HAS1BlackBox\data\telemetry.sqlite3" import-jsonl "C:\HAS1-Logs\server-20261024.jsonl" --site-id store-1

.venv\Scripts\python.exe timeline.py --database "%ProgramData%\HAS1BlackBox\data\telemetry.sqlite3" export --site-id store-1 --window-start "2026-10-24T19:00:00+09:00" --window-end "2026-10-24T19:05:00+09:00" > "%USERPROFILE%\Desktop\has1-timeline.jsonl"
```

`import-jsonl`은 자동 수집과 별도로 과거 파일을 가져올 때 사용합니다. 동일 ID·내용을
다시 가져와도 중복 저장하지 않으며 첫 관측 시각을 유지합니다. 동일 ID인데 내용이
다르면 충돌 오류로 처리합니다. 다른 사이트 ID는 거부합니다.

각 가져오기 명령은 기본 10,000행, 최대 100,000행으로 제한합니다(`--limit`). 장치 가져오기는
뒤에 행이 남으면 `truncated: true`를 반환합니다. 장치 하나의 다음 구간을 읽으려면
`--mac AABBCCDDEEFF --ring 0123456789abcdef --after-seq 10000`처럼 MAC·ring·순번을
함께 지정해야 합니다. `after-seq`만 사용하면 다른 장치의 기록을 건너뛰므로 거부합니다.
JSONL이 제한을 넘거나 중간에 잘못된 행이 있으면 오류가 나며 앞서 정상 저장한 행은
유지됩니다. 파일을 구간으로 나눠 다시 가져오면 중복 제거가 적용됩니다.

내보내기는 **시간 창/앵커 범위를 먼저 조회한 뒤** 출처 시각이 있는 기록, 앵커 적용
장치 후보, 정렬 불가능한 기록에 각각 독립적인 `--limit`을 적용합니다. 따라서 과거
서버 로그가 많다고 최신 장애 기록이나 시각 없는 장치 기록의 자리를 빼앗지 않습니다.
최대 출력 후보는 세 구분을 합쳐 `3 × limit`행입니다. 첫 줄의 `sections`에는 구분별
검사 수, `truncated`, `next_cursor`가 있습니다. **잘린 구분에서 어떤 이벤트가 보이지
않는다고 실제로 없었다고 판단하면 안 됩니다.**

다음 페이지는 해당 `next_cursor`를 `--timestamped-after`, `--device-after`,
`--unaligned-after`에 전달합니다. 다른 구분은 해당 커서를 전달하지 않으면 첫 페이지가
다시 출력되므로 이벤트 ID로 중복을 구분해야 합니다. 매장·시간 창·앵커를 동일하게
유지하십시오. 커서는 고정 정렬 순서의 이어 읽기 위치이며 DB 스냅샷이 아닙니다.
이전에 없던 과거 시각의 로그가 새로 들어오면 새 보고서를 시작해야 합니다.
현재 도구에는 자동 보존 기간/DB 삭제 정책이 없으므로 디스크 용량을 관리해야 합니다.

## 정규화 이벤트 형식

JSONL 한 줄에 이벤트 하나를 저장합니다. 원본 서버 파일에서는 `observed_utc`를
생략할 수 있으며 수집기가 실제로 읽은 UTC를 넣습니다. 게임 서버가 기록한
`occurred_utc`를 수집기 수신 시각으로 덮어쓰지 않습니다.

```json
{"schema_version":1,"site_id":"store-1","source":"game_server","source_id":"store-1-game-server","event_id":"unique-request-event-id","occurred_utc":"2026-10-24T10:00:00.123456+00:00","clock_quality":"unverified","uncertainty_ms":null,"mac":"AABBCCDDEEFF","device_id":null,"event_type":"http.request.completed","fields":{"http_status":200,"duration_ms":18},"raw":""}
```

| 필드 | 의미 |
|---|---|
| `site_id` | 매장 구분. 현재 `store-1`을 명시적으로 사용 |
| `source`, `source_id`, `event_id` | 출처와 출처 내부의 고유 이벤트 ID. 사이트와 함께 중복 판단 |
| `occurred_utc` | 원본 발생 시각. 모르면 `null` |
| `observed_utc` | Windows가 처음 관측한 시각. 발생 시각과 별개 |
| `clock_quality` | 시간의 근거. 명시적인 UTC 표기도 시계 정확성을 보장하지 않음 |
| `uncertainty_ms` | 발생 시각의 ±오차. 측정하지 않았으면 `null` |
| `mac`, `device_id` | 확인 가능한 장치 식별자. 모르면 `null` |
| `event_type`, `fields`, `raw` | 이벤트 종류, 구조화 정보, 원본 자료 |

UTC/오프셋이 명시된 ISO 8601만 정규화합니다. 예를 들어
`2027-01-01T00:10:00+09:00`은 `2026-12-31T15:10:00Z`와 같습니다. 오프셋 없는
`2026-11-01T01:30:00` 같은 시각은 DST 중복 시간 등을 구분할 수 없어 거부합니다.
Omada의 연도 없는 RFC3164 시각은 별도 파서가 가정을 붙여 처리하며 측정 시각으로
인정하지 않습니다. 세부 사항은 [Omada 시간 해석](OMADA.md#time-interpretation)을 보십시오.

`fields`와 `raw`는 각각 UTF-8 기준 최대 16 KiB입니다. 원본 Omada 바이트는
`fields.raw_encoding="base64"`와 base64 `raw`로 저장하여 잘못된 UTF-8도 보존합니다.
개별 정규화 이벤트/가져오기 행은 약 40 KiB로 제한합니다.

## 보고서에서 시간을 읽는 법

이벤트 출력은 다음 세 구분으로 나뉩니다. 알려진 발생 시각 순서 뒤에 정렬 불가능한
장치 로그를 별도로 둡니다. 시각 없는 로그의 관측 순서는 발생 순서가 아닙니다.

| `section` | 해석 |
|---|---|
| `aligned` | 측정/검증된 시계 품질과 오차 구간이 있음 |
| `timestamp_unverified` | UTC로 변환한 원본 시각은 있지만 실제 시계 오차를 검증하지 않음 |
| `unaligned` | 발생 UTC를 정할 근거가 없음. `observed_utc`는 별도로 표시 |

`clock_quality`가 `measured`, `synchronized`, `anchored` 중 하나이고 유한한
`uncertainty_ms`가 있어야 오차 구간을 만듭니다. 단순히 서버와 수집기가 같은 PC라는
이유만으로 이 값을 자동으로 바꾸지 않습니다. `synchronized` 문자열을 쓸 때에도
실제 시간 동기화/오차의 근거가 필요합니다.

시간 창과 오차 구간이 겹치면 `interval_overlap_candidate`로 표시합니다. 이것은
**같은 시기에 일어났을 가능성이 있는 조사 후보**이며 인과관계나 동일 사건의 증명이
아닙니다. 검증되지 않은 시각은 원본 시각 기준으로 창에 포함하되
`clock_not_verified`라고 표시합니다. 시각 없는 장치 이벤트는 창 안/밖을 결정할 수
없어 별도 `unaligned` 구분에 유지합니다.

## 장치 UTC 시계 측정 자료(선택 사항)

현재 펌웨어 기록에는 부팅 ID와 32비트 밀리초 uptime이 있으며 절대 UTC는 없습니다.
업로드가 지연될 수 있어 **`수신 UTC - uptime`으로 부팅 시각을 계산하면 안 됩니다.**
uptime은 약 49.7일마다 반복됩니다. 재부팅·로그 유실·순환 저장 때문에 인접한 두
기록만 보고 wrap 횟수를 추정하지 않습니다.

향후 실제 측정으로 특정 uptime이 UTC 어느 구간에 있었는지 확인한 경우에만 아래
앵커를 가져올 수 있습니다. 예제는 형식을 보여 주기 위한 가상 값이며 실제 장치의
측정 자료로 사용하면 안 됩니다.

```json
{"schema_version":1,"anchor_id":"measurement-001","site_id":"store-1","mac":"AABBCCDDEEFF","ring":"0123456789abcdef","boot":3,"uptime_ms64":101000,"utc_min":"2026-10-24T10:00:00.000Z","utc_max":"2026-10-24T10:00:00.020Z","origin":"Actual measurement method and evidence reference","seq_min":100,"seq_max":120,"record_uptime_wraps":0,"max_delta_ms":600000,"drift_ppm":1000}
```

```bat
.venv\Scripts\python.exe timeline.py --database "%ProgramData%\HAS1BlackBox\data\telemetry.sqlite3" import-anchors "C:\HAS1-Logs\measured-anchors.jsonl" --site-id store-1
```

요구되는 근거는 다음과 같습니다.

- 정확한 매장·MAC·ring·부팅 ID와 적용할 순번 범위.
- 측정한 64비트 확장 uptime과 그 순간의 UTC 최소/최대 시각. 구간 폭은 최대 60초.
- 적용할 기록들의 **명시적으로 확인한** uptime wrap 횟수. wrap을 가로지르면 순번
  범위를 나누고 각각의 wrap 횟수를 지정합니다.
- 측정 방법/근거를 적은 `origin`, 적용 거리 제한, 시계 드리프트 상한.

기본 적용 거리는 측정 uptime에서 ±10분이며 최대 ±1시간까지만 허용합니다.
드리프트 기본값 1000 ppm은 보수적인 **설정 가정**이며 이 ESP32의 실측 사양이
아닙니다. 실제 환경의 근거로 검토해야 합니다. 측정점과 멀어질수록
`거리(ms) × drift_ppm / 1,000,000`만큼 각 방향 오차가 커집니다.

앵커는 원본 로그를 수정하지 않고 보고서를 만들 때만 적용합니다. 다른 부팅·매장·ring,
범위 밖 기록, 오차 구간이 서로 모순되는 앵커는 정렬된 것으로 표시하지 않습니다.
앵커 ID는 매장 안에서 고유해야 하며 같은 ID의 측정 내용을 조용히 덮어쓰지 않습니다.

## 이번 구현의 검증 범위

합성 데이터로 KST/연도 경계/DST, 지연 업로드, 재부팅, wrap, 동일 MAC의 다른 매장,
중복/충돌, SQLite 내구성 설정, 읽기 전용 장치 가져오기, 행/크기 제한, 앵커 오차 확장을
검증했습니다. 실제 Omada 시계 오차·현장 UDP 손실·Windows 서비스 설치·게임 서버
배포와 실제 단절 재현은 별도 현장 확인이 필요합니다.

자동 수집과 자동 시간 기준점은 [EVIDENCE.md](EVIDENCE.md)를 참조한다. 자동 기준점은
PC UTC offset을 검증하지 않았으므로 `pc_unverified`로 분류하며, 측정한 왕복 구간을
절대 UTC 정확도로 해석하지 않는다. 기존 수동 `measured` anchor와 구분한다.

장기 수집으로 자동 앵커가 1,000개를 넘어도 타임라인 export는 전체 앵커를 메모리에
적재하거나 모든 앵커 쌍을 비교하지 않는다. 검증된 범위를 SQLite 임시 인덱스에
순차 반영하고, 각 기록에 적용되는 시간 구간의 교집합을 **행 제한 전에** 계산한다.
서로 모순되는 측정은 요청 시각 창 밖에 있어도 시각 미상 기록으로 남긴다.
앵커 상세 목록은 기록당 64개만 표시하며 `anchor_count`와
`anchor_details_truncated`를 제공한다. 교집합 계산에는 나머지 앵커도 모두 사용한다.
조회는 MQTT ACK 경로와 분리되어 있지만 큰 저널에서는 조회 시간이 늘 수 있다.

자동 측정의 `interval_in_collector_clock`은 수집 PC의 시계 기준 구간이다.
`pc_clock_overlap_unverified`는 이 구간이 조회 창과 겹친다는 뜻이며,
`interval_utc`는 계속 null이다. PC의 절대 UTC 오차를 검증했다는 뜻이 아니다.
