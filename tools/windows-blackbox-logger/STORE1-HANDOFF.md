# 1호점 현장 적용 순서

대상 PC는 게임 서버와 수집기를 함께 실행하는 `172.30.1.43`입니다. 이 문서는 실제 PC 설치 완료를 의미하지 않습니다. 게임이 없는 시간에 진행합니다.

1. [FIRST-INSTALL.md](FIRST-INSTALL.md)에 따라 Python·Mosquitto와 브로커·수집기 서비스를 설치합니다. 비공개 설치 자료의 기존 `private-provision.json`을 사용합니다. 장치 OTA와 암호가 맞아야 하므로 새 암호를 임의로 만들지 않습니다. 기존 설치가 있다면 첫 설치 스크립트를 재실행하지 않습니다.
2. 게임 서버에 사건 식별 기능이 포함된 `fuzzyline-core` 커밋 `b4a35bb39b94183d2f5af6f1853968e973539445` 또는 이를 포함하는 후속 버전을 적용합니다. 운영 서버의 현재 버전·실행 방법·DB 경로를 먼저 확인하고 백업합니다. 서버 저장소의 `docs/BLACKBOX-TELEMETRY.md`대로 실제 테마와 JSONL 출력 폴더를 설정합니다.
3. [README의 서버 로그·사건 수집 활성화](README.md#서버-로그사건-수집-활성화)를 따라 JSONL 입력과 사건 수집을 켭니다. 서버가 기록하는 폴더와 수집기가 읽는 폴더를 일치시키고 LocalService의 읽기 권한을 확인합니다. `game_monitor_snapshot`과 최신 `healthy` 상태를 확인합니다.
4. [OMADA.md](OMADA.md)에 따라 Omada의 로그 전송과 수집기 입력을 설정하고 실제 수신을 확인합니다. Omada는 `172.30.1.1`, 수신 PC는 `172.30.1.43`입니다. 포트·송신 장비 주소·타임존은 실제 설정에 맞춥니다.
5. 아래 명령으로 Slack·Notion을 연결합니다. 숨김 입력창에 Webhook과 API 토큰을 입력합니다. Notion 연결은 `HAS1 BlackBox Logger`이며, 먼저 Notion에서 `HAS1 장애 관리` 페이지의 연결 메뉴에 이 연결을 추가합니다. 설정에는 `HAS1 장치 이벤트` DB의 data source ID를 넣습니다. 아래 ID는 비밀 토큰이 아닙니다.

```powershell
Set-Location 'C:\Program Files\HAS1 BlackBox'
.\configure-notifications.ps1 -EnableSlack -EnableNotion `
  -NotionDataSourceId '5fa07803-ec16-4e79-a60a-489a5d52acf8'
Restart-Service HAS1BlackBoxCollector
```

설치 위치를 바꿨다면 실제 경로에서 실행합니다. 설정 도구는 이벤트 DB와 연결된 장애 DB의 읽기 권한·속성·상태 옵션을 조회한 뒤에만 설정을 저장하며, 시험 메시지나 페이지는 만들지 않습니다. DB 링크를 쓰려면 위 ID 인수 대신 `-NotionDatabaseUrl 'https://app.notion.com/p/7ecbcc994d4d4287a2fbdb5664db5486'`를 사용할 수 있습니다. 예전 `HAS1 장치 장애 이력` DB(`4030bd38-10bf-8090-aa3f-000b6435ed61`)는 이 버전의 수집기와 호환되지 않습니다. 새 수집기 배포와 위 재설정을 같은 작업 시간에 함께 진행합니다.

끊김과 복구를 한 Slack 스레드로 묶으려면 `-SlackChannelId '<#has1-device-alerts 채널 ID>'`를 추가하고 Webhook 대신 `xoxb-` bot 토큰을 입력합니다. 앱 준비는 [NOTIFICATIONS.md](NOTIFICATIONS.md#준비)를 따릅니다.

## Notion 표시

장치 끊김은 `HAS1 장치 이벤트`에 한 행으로 기록되고, 같은 게임 세션의 행끼리 `HAS1 장애 (게임 세션)` 한 행에 묶입니다. 원인 분석은 사람이 `HAS1 문제 (근본 원인)`에 기록합니다.

| 사건 원본 상태 | 이벤트 `상태` | 의미 |
| --- | --- | --- |
| offline | 장애중 | 게임 중 서버 응답 중단 |
| recovered | 복구됨 | 실제 재접속 확인 |
| monitoring_ended | 미복구 종료 | 감시 종료, 복구 여부 확인 필요 |

`미복구 종료`는 장치가 복구됐다는 뜻이 아닙니다. 수집기가 쓰는 속성·상태 옵션을 임의 변경하면 사전 검사에 실패할 수 있으므로 [NOTIFICATIONS.md](NOTIFICATIONS.md)를 확인합니다.

## 현장 합격 기준

- 시험 장치의 MQTT 로그가 SQLite에 저장되고 ACK를 받음.
- 서버 JSONL, 장치 로그, Omada 로그가 각각 들어오며 [TIMELINE.md](TIMELINE.md)의 시간 기준과 오차를 확인함.
- 정상 게임 종료 후 배터리 교체에서 장애 알림이 발생하지 않음.
- 운영자가 승인한 시험 게임에서 통신 단절·재접속을 만들고, 동일 사건 ID의 장치 이벤트 한 행이 갱신되고 그 게임 세션의 장애 행에 연결되며 Slack에 단절·복구가 구분되어 도착함.
- 복구 없이 게임을 종료한 경우 복구 알림으로 오인하지 않고 감시 종료로 기록됨.
- PC 재부팅 후 서비스가 자동 실행되고 수집이 재개됨.

외부 전송 시험은 현장 담당자가 진행합니다. 서비스 Running이나 사전 검사 통과만으로 실제 장치 수집·Slack 발송·Notion 저장을 검증한 것은 아닙니다. 이번 연결 복구·버튼 계측 변경은 글러브 v19·발전기 v32 적용이 필요합니다. v18·v31에는 이 수정이 없습니다. 기존 파티션과 MQTT 인증 설정은 유지하며, Beetle 펌웨어는 이번 변경 대상이 아닙니다. 먼저 시험 장치 한 대씩에서 AP 단절·복귀와 버튼/진동 기록을 확인한 뒤 나머지 장치에 적용합니다. OTA 진행 중이거나 발전기의 완료 요청 결과가 불확실한 경우에는 복구 재시작이 보류될 수 있습니다.
