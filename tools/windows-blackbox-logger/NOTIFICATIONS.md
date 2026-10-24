# Slack·Notion 장애 알림 설정

이 기능은 기본 비활성화입니다. Windows 수집기의 사건 관리와 별도 알림 작업자가 실행하며, 장치 MQTT 저장·ACK 경로에서는 외부 HTTP 요청을 하지 않습니다. 게임 중 참가 글러브의 서버 통신 중단 사건을 사용합니다. MQTT 로그가 잠시 없거나 게임 종료 후 배터리를 교체했다는 이유로 알림을 만들지 않습니다. 서버 사건 수집을 먼저 설정하고 실제 로그 유입을 확인하세요.

## 준비

- Slack: 채널 이름은 `#has1-device-alerts`, 앱 이름은 `HAS1 BlackBox`를 권장합니다. 두 방식 중 하나를 사용합니다.
  - **Bot 토큰(권장)**: 앱의 Bot Token Scopes에 `chat:write`만 추가하고 워크스페이스에 설치한 뒤 `xoxb-`로 시작하는 Bot User OAuth Token을 받습니다. 채널에서 `/invite @HAS1 BlackBox`로 앱을 초대하고, 채널 정보 하단의 **채널 ID**(`C`로 시작)를 확인합니다. 끊김 알림이 카드로 올라오고, 복구되면 같은 카드가 🟢 복구됨으로 바뀌며 그 스레드에 복구 답글이 달립니다. 복구 확인 없이 감시가 끝나면 카드만 ⚪ 모니터링 종료로 바뀌고 답글은 달지 않습니다.
  - **Incoming Webhook**: 이 채널에 Webhook을 발급합니다. URL 자체가 암호입니다. 메시지 ID가 돌아오지 않아 복구 알림은 별도 메시지로 올라옵니다.
- Notion: 데이터베이스 이름은 `HAS1 장치 장애 이력`, 내부 연결 이름은 `HAS1 BlackBox Logger`를 권장합니다. 내부 연결에 대상 데이터베이스의 읽기·추가·수정 권한을 부여합니다. 데이터베이스 아래 **data source ID**를 확인합니다. API 버전 `2025-09-03`의 데이터 소스 API를 사용하므로 기존 database ID와 혼동하면 안 됩니다.
- Notion 데이터 소스에는 아래 속성을 정확한 이름과 유형으로 준비합니다. 수집기는 스키마를 자동 변경하지 않습니다.

| 속성 | Notion 유형 |
| --- | --- |
| Name | 제목(title) |
| Incident ID | 텍스트(rich_text) |
| Status | 상태(status), 또는 기존 텍스트(rich_text) |
| Summary | 텍스트(rich_text) |

`Status`는 Notion의 상태(status) 유형을 지원합니다. 일반 텍스트(rich_text)로 만든 기존 DB도 그대로 사용할 수 있습니다. 상태 유형에서는 아래 이름의 옵션이 **이미 존재해야** 하며 대소문자와 공백까지 정확히 일치해야 합니다. 수집기는 옵션을 만들거나 DB 속성을 변경하지 않습니다.

| 사건 원본 상태 | Notion 상태 옵션 | 의미 |
| --- | --- | --- |
| `offline` | `In progress` | 통신 장애 발생, 조사 진행 대상 |
| `recovered` | `Done` | 서버에서 통신 복구 확인 |
| `monitoring_ended` | `Not started` | 복구 확인 전 감시 종료, 별도 확인 필요 |

감시 종료는 복구가 아니므로 `Done`으로 표시하지 않습니다. 이 매핑은 고정이며 한국어 등으로 옵션 이름을 바꾼 DB는 이름을 맞추거나 `Status`를 기존 텍스트 유형으로 사용해야 합니다. 원본 상태는 `Summary`의 `status:` 항목에도 보존합니다. 텍스트 유형에서는 원본 상태를 그대로 기록합니다.

설정 시와 매 전달 작업 전에 데이터 소스를 읽어 `Name`·`Incident ID`·`Summary`의 유형과 `Status`의 유형·옵션을 검증합니다. 다른 필드는 무시하므로 기존 자동 번호 속성은 동작에 영향을 주지 않습니다. 지원하지 않는 상태·누락된 필드·옵션 불일치는 쓰기 전에 중단하며, 원격 오류 본문을 포함하지 않는 오류 코드만 남깁니다.

데이터베이스의 연결 메뉴에서 위 내부 연결을 추가해야 해당 연결의 API 토큰으로 조회할 수 있습니다. DB 페이지 URL을 받으면 ID를 확인할 수 있지만, API 버전 `2025-09-03`의 `Retrieve a database` 응답에 있는 `data_sources` 목록에서 실제 데이터 소스 ID를 확인해야 합니다. 데이터 소스가 여러 개면 이 네 속성을 가진 대상을 명시적으로 선택합니다. 토큰이나 Webhook URL은 채팅에 보내지 말고 현장 설정의 숨김 입력창을 사용하세요.

`Incident ID`로 기존 기록을 조회해 사건당 하나의 페이지를 만들고 상태 변경 시 갱신합니다. 중복된 ID의 페이지가 이미 있으면 임의 선택하지 않고 오류로 남깁니다. 여러 수집기가 같은 데이터 소스에 동시에 같은 사건을 쓰지 않도록 운영하세요. Notion은 이 속성에 고유 제약을 보장하지 않습니다.

## Windows에서 활성화

1호점 설치 대상과 순서는 [STORE1-HANDOFF.md](STORE1-HANDOFF.md)에 정리되어 있습니다.

수집기를 설치하고 [README의 사건 수집 활성화 절차](README.md#서버-로그사건-수집-활성화)에 따라 실제 `%ProgramData%\HAS1BlackBox\config.json`의 서버 로그 입력과 사건 기능을 먼저 활성화합니다. `telemetry.incidents.database`는 `data/incidents.sqlite3` 또는 해당 ProgramData 폴더의 절대 경로로 지정해야 LocalService가 쓰기 가능한 데이터 폴더를 사용합니다. 관리자 PowerShell에서 설치 폴더의 다음 스크립트를 실행합니다.

```powershell
# Slack bot 토큰 방식(복구가 끊김 카드의 스레드로 이어짐). 채널 ID는 비밀이 아닙니다.
.\configure-notifications.ps1 -EnableSlack -SlackChannelId 'C0123456789'
# Slack Incoming Webhook 방식:
.\configure-notifications.ps1 -EnableSlack
# 또는 Slack과 Notion을 함께 사용(DB 페이지의 복사한 링크):
.\configure-notifications.ps1 -EnableSlack -EnableNotion -NotionDatabaseUrl 'https://www.notion.so/복사한DB링크'
# 이미 데이터 소스 ID를 아는 경우 URL 대신 지정:
.\configure-notifications.ps1 -EnableSlack -EnableNotion -NotionDataSourceId '데이터-소스-UUID'
Restart-Service HAS1BlackBoxCollector
```

Slack bot 토큰·Webhook URL과 Notion 토큰은 숨김 입력창에서 입력합니다. `-SlackChannelId`를 지정하면 Webhook 대신 bot 토큰을 묻습니다. 다른 채널 옵션과 함께 쓸 때도 그대로 붙이면 됩니다. 명령줄 인수·환경변수·JSON 설정 파일에 직접 쓰지 않습니다. 스크립트는 Windows DPAPI(LocalMachine)로 암호화한 새 `notifications-*.dpapi` 파일을 만들며, ACL을 SYSTEM·관리자 전체 접근 및 LocalService 읽기로 제한합니다. 이 파일은 같은 Windows PC에서만 복호화됩니다. 새로 설정할 때 선택한 채널만 활성화되므로 기존 두 채널을 유지하려면 두 옵션을 모두 지정하세요. 이전 암호 파일은 자동 삭제하지 않으므로 정상 동작 확인 후 현재 `credential_file`에 지정되지 않은 파일만 삭제할 수 있습니다.

설정 위치는 다음과 같습니다. 실제 ID 이외의 비밀은 포함하지 않습니다.

```json
{
  "telemetry": {
    "incidents": {
      "enabled": true,
      "database": "data/incidents.sqlite3",
      "notifications": {
        "enabled": true,
        "credential_file": "notifications-생성된파일명.dpapi",
        "slack": {"enabled": true, "channel_id": "C0123456789"},
        "notion": {"enabled": true, "data_source_id": "12345678-1234-1234-1234-123456789abc"}
      }
    }
  }
}
```

위 조각으로 전체 설정 파일을 덮어쓰지 마세요. 설정 스크립트가 기존 설정에 병합합니다. `-NotionDatabaseUrl`을 사용하면 공식 Notion API에 읽기 전용 GET 요청을 보내 데이터 소스 ID를 확인합니다. 하나일 때만 자동 선택하며, 여러 개면 명시적 선택을 요청합니다. `notion.so`·`www.notion.so`·`app.notion.com`의 복사한 DB 링크를 지원하며, 입력한 링크로 직접 접속하지 않고 ID만 추출합니다. URL이나 데이터 소스 ID 중 어느 방식이든 저장 전에 읽기 전용 스키마 검증을 수행합니다. 검증에 실패하면 기존 알림 설정을 유지합니다. 설정 도구는 공통 실패 안내를 표시합니다. 실행 중 발송 대기열에 남는 오류 코드에서 `notion_schema_mismatch`는 필드 이름·유형, `notion_status_options_mismatch`는 위 상태 옵션 이름을 확인해야 한다는 뜻입니다. `http_401`·`http_403`·`http_404`는 토큰과 대상 DB 접근 권한을 확인하세요. 설정 시에는 외부 테스트 메시지나 Notion 기록을 만들지 않습니다. 최초 연결 검증은 운영 승인된 시험 사건으로 진행하고 전송 대기 상태를 확인합니다. 실제 Slack·Notion 계정으로의 전송은 현장 설정 후 검증해야 합니다.

## 전달 보장과 실패 처리

- Slack 알림은 매장·장치·MAC·한국 시각·무응답/장애 지속 시간을 담은 카드입니다. 값이 없거나 `unknown`인 항목은 표시하지 않습니다. 본문은 plain text라 장치/서버 문자열이 `@channel` 같은 멘션 명령이 되지 않습니다.
- Bot 토큰 방식은 끊김 카드의 메시지 ID(`ts`)를 대기열 `remote_id`에 저장합니다. 복구 시 먼저 원본 카드를 수정하고(`chat.update`, 재시도 안전), 성공한 뒤에만 스레드 답글을 보냅니다(`chat.postMessage`). 그래서 수정 실패는 중복 답글 없이 재시도하고, 답글 전송 결과를 알 수 없으면 manual_review로 남깁니다. `not_in_channel`·`channel_not_found`·`invalid_auth` 같은 설정 오류는 `slack_<오류명>` 코드의 영구 오류로 기록합니다. Slack이 일시 오류로 안내하는 `internal_error`·`fatal_error`·`request_timeout`은 카드 수정이면 재시도하고, 새 메시지 전송이면 이미 올라갔을 수 있어 manual_review로 남깁니다. `service_unavailable`은 처리되지 않은 요청이라 둘 다 재시도합니다. 원본 카드가 삭제되어 수정할 수 없으면 복구 카드를 새 메시지로 올립니다. 수정과 답글은 한 번의 약 10초 HTTP 예산을 나눠 쓰므로, 수정이 느리면 답글 결과가 불확실해져 manual_review로 남을 수 있습니다(중복 전송은 없음). Webhook 방식은 복구만 별도 메시지로 알립니다.
- Notion에는 같은 사건 ID로 상태·요약을 갱신합니다. 상세 원본 로그는 PC SQLite에 유지하고 외부에는 선택된 사건 필드만 전송합니다. Notion은 원본 로그의 영구 백업이 아닙니다.
- HTTP는 허용된 HTTPS 호스트에만 연결하고 리디렉션을 따르지 않습니다. 한 전달 작업은 DNS·응답 읽기를 포함해 최대 약 10초의 HTTP 예산을 사용하며, 응답은 64KiB로 제한합니다.
- 429/529는 `Retry-After`를 적용합니다(최대 24시간). 대부분의 4xx는 설정 오류로 분류하고 무한 재시도하지 않습니다. 외부 오류 본문·URL·토큰은 오류 기록에 넣지 않습니다.
- Slack POST 또는 Notion 페이지 생성 직후 응답이 유실되면 실제 저장 여부를 알 수 없습니다. 이 경우 **unknown/manual_review** 상태로 남겨 자동 재전송하지 않습니다. 운영자가 실제 채널/페이지와 사건 ID를 확인해야 합니다. Notion 스키마 읽기·사건 조회와 기존 페이지의 동일 속성 갱신은 안전한 재시도 대상으로 구분합니다. 일시적인 스키마 응답 오류는 쓰기 없이 재시도하며, 스키마 유형·옵션 불일치는 영구 설정 오류로 남깁니다. 설정을 수정한 뒤에는 수집기 DB의 실패한 대기열도 운영 절차에 따라 확인해야 하며, 설정 수정만으로 이전 실패 사건이 자동 재전송되지는 않습니다.
- SQLite 대기열과 사건 ID를 사용해도 외부 API에서 **정확히 한 번 전달**이 보장되는 것은 아닙니다. Slack Incoming Webhook에는 조회·수정용 메시지 ID가 응답으로 반환되지 않습니다. Bot 토큰 방식도 메시지 조회 권한은 쓰지 않으므로 전송 결과가 불확실한 메시지를 자동으로 찾아 정리하지는 않습니다.
- 알림을 중지하려면 `telemetry.incidents.notifications.enabled`를 `false`로 바꾸고 서비스를 재시작합니다. 사건·원본 로그 수집은 별도로 유지됩니다.

## 공식 근거

- [Slack Incoming Webhooks](https://docs.slack.dev/messaging/sending-messages-using-incoming-webhooks/): 비밀 URL, JSON POST, 응답 및 메시지 관리 제한.
- [Slack chat.postMessage](https://docs.slack.dev/reference/methods/chat.postMessage) · [chat.update](https://docs.slack.dev/reference/methods/chat.update): `chat:write` 범위, `ts`·`thread_ts`, 오류 응답.
- [Notion Retrieve a data source](https://developers.notion.com/reference/retrieve-a-data-source): 읽기 전용 속성·상태 옵션 검증.
- [Notion Page property values](https://developers.notion.com/reference/page-property-values): 기존 상태 옵션 이름으로 상태 속성 갱신.
- [Notion Query a data source](https://developers.notion.com/reference/query-a-data-source): 데이터 소스별 필터 조회.
- [Notion 2025-09-03 업그레이드](https://developers.notion.com/guides/get-started/upgrade-guide-2025-09-03): database/data source 분리.
- [Notion 요청 제한](https://developers.notion.com/reference/request-limits): Retry-After, 재시도 제한, 불확실한 쓰기 결과 처리.

단위 검증은 HTTP 대역과 합성 자격 증명만 사용합니다. 일회용 Windows 서비스 CI는 서비스가 중지된 동안 실제 설정 스크립트의 숨김 입력을 합성 값으로 대체하여 DPAPI 왕복·제한된 ACL·기존 설정 보존을 검증합니다. 이후 알림 비활성 설정을 복원한 다음 서비스를 시작하므로 이 시험에서 외부 전송은 발생하지 않습니다. 실제 Slack 메시지 발송이나 Notion 페이지 작성은 수행하지 않습니다.
