# 장애 증거 보완 (2026-10-24)

이 기능은 수집기와 서버가 함께 있는 1호점 Windows PC용이다. 장치의 게임 HTTP,
OTA, 기존 `ACK1` 형식은 그대로다. 진단 기능은 기존 장치 배치의 SQLite COMMIT 성공을
대신하지 않으며, 별도 DB 장애가 장치 ACK를 기다리게 하지 않는다.

## 설정

`config.store1.example.json`의 다음 기능은 기본값이 모두 `false`다.
원하는 항목만 실제 수집기 `config.json`에서 켜고 서비스를 다시 시작한다.

- `telemetry.device_import.enabled`: 같은 프로세스의 `database`를 읽기 전용으로 읽어
  `telemetry.database`에 반영한다. 별도의 원본 파일/사이트를 자동 추정하지 않는다.
- `telemetry.clock_probe.enabled`: 현재 연결된 장치 진단 MQTT 세션에서만 시간 기준점을
  요청한다. 새 펌웨어가 필요하다. 기존 MQTT 계정/ACL/토픽을 그대로 사용한다.
- `telemetry.gateway_udp.enabled`: `127.0.0.1:5515`에서 PHP 앞단 진단을 받는다.
  `source_id=store-1-php-gateway`, `site_id=store-1`이 송신기와 일치해야 한다.
  외부 NIC에는 열지 않는다. PHP 설정도 별도로 켜야 한다.

`telemetry` 설정이 존재하면 수집 시작/중지와 약 30초 주기의 상태 기록은 항상 남긴다.
Omada/서버 JSONL 설정은 기존과 같다. 설치 스크립트가 새 Python 모듈도 복사한다.
Windows 서비스의 계정이나 게임 폴더 접근 권한을 추가 변경하지 않는다.

## 자동 장치 반영과 보존

MAC/ring별 증분 진행 위치는 통합 DB에 저장한다. 매 회차에 새 seq를 먼저 읽고,
별도의 제한된 재검사 페이지로 늦게 들어온 작은 seq도 찾는다. 재시작 시 목적 DB에
이미 저장된 event ID로 중복 제거한다. 커서는 이벤트 COMMIT 뒤에 갱신되므로 장애가
나면 재검사할 뿐이다. 매 회차 한 MAC/ring, 새 기록 최대 128행과 재검사 최대 128행을
처리한다. 기기가 많거나 원본이 오래되면 재검사에는 시간이 걸릴 수 있다.

원본 DB는 계속 장치 ACK의 근거다. 반영 속도나 기록이 없는 기간으로 장치 오프라인을
판정하지 않는다. 게임·OTA 중 업로드가 제한되고 과거 기록은 나중에 도착할 수 있다.

## 시간 측정의 의미

수집기는 COMMIT 후 일반 ACK 직전에 `T1 <128-bit random token>`을 보낸다.
장치는 같은 세션에서 token/MAC/ring/현재 boot/64비트 uptime/현재 uptime epoch에
저장된 seq 범위를 응답한다. 일반 ACK를 시간 응답 때문에 미루지 않는다.
응답이 도착하지 않으면 측정값을 만들지 않는다.

요청 전후 PC 시각으로 구간을 만들고 RTT 2초 초과, wall/monotonic 변화 차이 50ms 초과,
잘못된 token/identity/epoch, 재사용 응답을 거절한다. 장치별 요청은 60초에 최대 1회이며
미완료 요청은 최대 256개, 저장 대기 anchor는 최대 64개다. PC 시각 샘플링 여유 50ms를
양끝에 더한다. 순환 횟수와 boot가 다른 기록에는 적용하지 않는다.

**PC 자체의 UTC 정확도를 검증한 것은 아니다.** 자동 측정은 `pc_unverified`이며
`alignment.interval_scope=collector_pc_clock`, `reference_clock_offset_ms=null`로 표시한다.
계산된 구간은 같은 PC 시계에 대한 상대 구간이다. 절대 UTC 동시성을 인증하지 않는다.
드리프트 1000ppm은 보수적으로 선택한 가정이며 실측 규격이 아니다. 기준점에서 최대
30분까지만 사용한다. 게임이 길거나 과거 boot 기록만 남으면 여전히 정렬할 수 없다.
수집기는 MQTT 세션을 강제로 열거나 게임 중 추가 네트워크 통신을 시작하지 않는다.

## 수집 상태와 시각 필드

- `collector_started`, `collection_health`, `clock_step`, `collector_stopping`을 UTC로 저장한다.
- MQTT 연결/구독 상태와 마지막 변경 이유, 마지막 배치 도착/COMMIT 완료 시각,
  큐 깊이, worker 생존 여부, probe 결과, Omada/PHP/JSONL 오류·드롭, 디스크 여유,
  장치 반영 커서(최대 32개; 초과는 표시)를 포함한다.
- PC 시계와 monotonic 차이가 1초 넘게 변하면 별도 기록한다. 이것은 NTP 동기화 증명이 아니다.
- 서비스/PC가 꺼진 동안 자체 heartbeat도 없으며 UDP는 무손실을 보장하지 않는다.
  디스크가 부족하면 상태 기록 자체도 실패할 수 있다. 이후 상태의 오류 카운터와
  시간 공백을 확인한다. 동일 PC 전체 장애의 확증에는 OS 이벤트나 외부 관측이 필요하다.

원본 SQLite는 v1→v2 자동 추가 마이그레이션을 한다. 기존 바이트·식별키·received_utc는
유지하고, 과거 행의 새 필드는 NULL로 둔다. 재수신도 최초 보존 시각을 덮어쓰지 않는다.

- `arrival_utc`: MQTT 콜백에서 큐에 받아들인 시각
- `received_utc`: 호환성을 유지한 기존 저장 처리 시작 시각
- `queue_latency_ms`: monotonic으로 잰 수신→저장 처리 시작 지연
- `persisted_utc`: 트랜잭션 내부의 **COMMIT 요청 직전 시각**. 실제 완료 시각은 이보다 늦다.
이 세부 수신/저장 시각은 원본 DB와 `export.py`에서 조회한다. 기존 통합 타임라인의 장치
`observed_utc`는 호환성을 위해 `received_utc`를 유지하며, 세부 큐 지연 필드를 모두 복제하지 않는다.

- `last_commit_completed_utc`: 상태 기록의 실제 COMMIT 반환 후 시각. 각 개별 행의 완료 시각은 아니다.

## 조회

원본 export CLI는 이제 최신 수집 시각부터 출력한다. JSONL 행은 stdout,
잘림 여부와 다음 페이지 커서는 stderr에 출력한다. 프로그램에서 `export()`를 호출하면
메타데이터가 반환값이며 기존 기본 순서(MAC/ring/seq)는 유지된다.

```powershell
python export.py data/blackbox.sqlite3 --limit 1000 1> recent.jsonl 2> recent-page.json
python export.py data/blackbox.sqlite3 --limit 1000 --after "<next_cursor>" 1> next.jsonl 2> next-page.json
python export.py data/blackbox.sqlite3 --window-start "2026-10-24T13:00:00+09:00" --window-end "2026-10-24T14:00:00+09:00"
```

위 시간 창은 **수집/저장 시각**이다. 장치 발생 시각 창은 anchor를 이용하는 `timeline.py
export`를 사용한다. 어느 조회도 raw bytes를 삭제/수정하지 않는다. `named_values`에는
새 증거 태그의 이름/단위, signed 원본과 uint32 변환을 병기하며 모르는 태그는 unknown이다.
`device.boot.actual_reset_reason`과 과거 last_reason을 혼동하지 않는다. ADC pin mV는
배터리 mV와 별개다. 발전기의 실제 전압 계측이 없는 점은 코드로 해결할 수 없다.

## Omada CSV 보충

실제 Omada 버전의 CSV 헤더를 아직 검증하지 않았으므로 컬럼 이름을 가정하지 않는다.
사용자가 실제 파일에 맞춰 `message`, 선택적 `timestamp/client_mac/ap_mac` 컬럼을 매핑한다.
CSV 시각은 명시적 UTC offset이 있어야 한다. 없으면 시간 매핑을 생략하여 unknown으로
보존하거나 사본에서 검증한 시간대만 적용한다. 원본 CSV 셀을 보존하며 이를 Syslog 원문으로
주장하지 않는다. 최대 파일 20MiB, 셀/행 8KiB, 기본 10,000행이다. 잘리면 결과에 표시된다.

```powershell
python import_omada_csv.py actual.csv --database data/telemetry.sqlite3 --site-id store-1 --source-ip 172.30.1.1 --mapping '{"message":"<실제 메시지 헤더>","timestamp":"<실제 offset 포함 시각 헤더>"}'
```

동일 파일/행은 SHA256+행 번호로 중복 제거한다. 매핑이 달라 같은 행의 사실이 바뀌면
충돌로 중지한다. Syslog의 RSSI/reason/channel/client_ip/ap_ip/dhcp_type은 명시적인
`[label:value]`가 있을 때만 배열로 보존한다. 실제 원문에 없는 신호/사유를 만들어내지 않는다.
