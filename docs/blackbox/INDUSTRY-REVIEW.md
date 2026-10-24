# 사건 식별·알림 설계와 업계 방식 비교

검토일: 2026-10-24. 대상: 1호점의 기존 장치 블랙박스, Node 게임 서버, Windows 수집기. 아래는 공식 문서와 이번 환경을 비교한 설계 판단이며, OpenTelemetry 호환 제품 인증이나 전체 분산 추적 구현을 의미하지 않는다.

## 확인한 업계 방식

| 방식 | 공식 근거 | 이번 적용 |
|---|---|---|
| 요청·출처·시간으로 로그 연결 | [OpenTelemetry Logs](https://opentelemetry.io/docs/specs/otel/logs/), [Logs Data Model](https://opentelemetry.io/docs/specs/otel/logs/data-model/) | 매장·서버 부팅·게임 회차·장치 MAC을 기록하고 발생 시각과 수집 시각을 구분한다. |
| 요청 전체에 추적 문맥 전달 | [W3C Trace Context](https://www.w3.org/TR/trace-context/) | 기존 Node 요청 ID는 Node 내부 요청 식별자로 유지한다. 사건 ID를 trace_id라고 부르거나 장치·PHP·AP까지 같은 요청이라고 추정하지 않는다. |
| 알림 중복 제거·그룹화·억제 | [Prometheus Alertmanager](https://prometheus.io/docs/alerting/latest/alertmanager/), [그룹 대기와 억제 설정](https://prometheus.io/docs/alerting/latest/configuration/) | 같은 사건의 최초 장애/복구를 구분한다. 게임 종료·참가 해제를 복구로 잘못 표시하지 않고, 최신 서버 상태가 없으면 장치별 신규 알림을 보류한다. |
| 사건 저장과 알림 예약을 같은 트랜잭션에 기록 | [AWS transactional outbox](https://docs.aws.amazon.com/prescriptive-guidance/latest/cloud-design-patterns/transactional-outbox.html) | SQLite 사건 기록과 발송 대기열을 함께 저장한다. 재시작 후 재처리는 동일 식별자로 중복을 제한한다. |
| 장애 시 전송 대기열을 디스크에 보존 | [OpenTelemetry Collector resiliency](https://opentelemetry.io/docs/collector/resiliency/) | 외부 전송 실패가 MQTT 로그 저장·ACK를 막지 않게 분리한다. 용량·재시도·실패 상태를 명시한다. |
| 실제 영향에 근거한 알림 | [Google SRE: Monitoring Distributed Systems](https://sre.google/sre-book/monitoring-distributed-systems/) | 게임 중 서버가 판단한 통신 단절을 알린다. 정상 배터리 교체·MQTT 미수신만으로 장애를 추정하지 않는다. |

## 현재 환경에서의 선택

현재는 기존 SQLite와 Windows 서비스에 사건 처리·발송 작업을 추가한다. 추가 브로커나 대규모 모니터링 스택을 설치하지 않아 운영 대상을 줄일 수 있다. 이는 이 매장 구성에 대한 판단이며 성능 우위를 측정한 결과는 아니다. 로그 수집과 사건 판단, 외부 전송은 각각 독립된 단계로 취급한다.

식별자는 역할을 분리한다. `game_session_id`는 게임 한 회차, `incident_id`는 한 장애의 생명주기, `request_id`는 요청 처리, 서버 `boot_id`와 장치 `boot`는 각 재시작 구간이다. 게임·사건 ID는 메트릭의 무제한 라벨로 사용하지 않는다. [Prometheus의 라벨 수 지침](https://prometheus.io/docs/practices/instrumentation/)도 차원이 큰 정보는 별도 분석 시스템으로 다루도록 권고한다.

Omada와 장치 과거 로그는 사건 ID를 직접 알지 못한다. 확인된 MAC과 시각 구간이 맞으면 관련 후보로 연결하고, 근거·시계 불확실성을 보존한다. 발생 시각이 없는 장치 로그의 도착 시각을 장애 발생 시각으로 대신하지 않는다. 같은 시각의 로그가 같은 원인이라는 보장은 없다.

최신 게임 상태 스냅샷은 이벤트 누락과 재시작을 보완한다. 완전하고 최신인 같은 서버 부팅·게임 회차의 상태에서만 신규 알림을 허용한다. 오래된 로그 재처리, 게임 종료, 서버 상태 미확인은 이력/보류로 구분한다. 실제 재접속과 감시 종료는 다른 상태다.

## 더 나은 확장안과 적용 시점

1. **여러 매장의 통합 운영:** 중앙 OpenTelemetry 로그 수집과 Prometheus/Alertmanager 도입을 검토한다. 검색·알림 운영을 표준 도구로 모을 수 있지만 서버·보존 정책·업데이트 관리가 추가된다. 현재 코드를 이미 이 스택과 호환된다고 표현하지 않는다.
2. **정확한 요청별 원인 추적:** PHP→Node 구간부터 검증된 W3C `traceparent` 전파를 추가할 수 있다. 장치까지 추적하려면 별도 펌웨어·프로토콜 검증이 필요하므로 이번에는 바꾸지 않는다.
3. **PC 전체 장애 감시:** 수집기와 게임 서버가 같은 PC라 그 PC가 꺼지면 스스로 Slack 알림을 보낼 수 없다. 별도 PC/NAS/외부 감시자가 하트비트를 확인하는 구성이 추가로 필요하다. 이번 구현 대상에는 외부 감시자를 포함하지 않는다.
4. **경보 폭주 억제:** 서버/AP 장애가 확인될 때 관련 글러브 사건을 상위 사건 아래 묶는 기능을 확장할 수 있다. AP와 장치의 연결 관계 및 시각이 확인되지 않은 상태에서 개별 사건을 자동으로 같은 원인으로 합치지 않는다.

## 보장 범위

Slack·Notion 같은 외부 API에 대해 정확히 한 번 전달을 보장할 수 없다. 로컬 중복 제거와 외부 전송 결과 불명확은 다른 문제다. 전송 후 응답을 받기 전에 끊긴 경우에는 성공으로 기록하지 않고, 무조건 재전송해 알림을 늘리지도 않는다. 불명확한 전송은 별도 확인 상태로 남긴다. Notion은 사건 ID로 기존 기록을 확인한 후 갱신한다.

공식 문서의 개념을 적용하되 외부 전송 실증, 실제 Windows 업그레이드와 매장 게임·배터리 교체 검증은 별도로 기록해야 한다.
