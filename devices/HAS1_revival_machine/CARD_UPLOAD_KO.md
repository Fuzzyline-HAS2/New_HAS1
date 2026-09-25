# Telnet 생명칩 업로더 사용법

생명장치의 PN532에 카드를 대고 Telnet 명령으로 NDEF URL 또는 텍스트를 기록합니다. 이 문서는 **생명장치 펌웨어 v68** 기준입니다. 연결만으로 기록되지 않으며, `write` 명령 한 번에 카드 한 장만 처리합니다.

## 1. 준비 및 접속

1. 컴퓨터와 생명장치를 같은 네트워크에 연결합니다.
2. 서버 관리 화면 또는 기존 서버 제어 방법으로 대상 장치의 `device_state`를 `card-upload`로 변경합니다. Academy 테스트 장치 이름은 `AR`입니다.
3. **생명장치 IP의 TCP 23번 포트**에 Telnet으로 접속합니다. 서버 IP로 접속하는 것이 아닙니다.

| 구분 | Academy 테스트 당시 예시 | 용도 |
| --- | --- | --- |
| 서버 | `172.30.1.43` | `AR`의 `device_state` 변경 |
| 생명장치 AR | `172.30.1.45` | Telnet 업로더 접속 |

IP는 네트워크 설정에 따라 바뀔 수 있습니다. 장치 부팅 로그의 `Telnet ready: <IP>:23` 또는 현재 장치 주소를 확인합니다.

Telnet 클라이언트가 설치된 터미널에서는 다음과 같이 접속합니다.

```sh
telnet 172.30.1.45 23
```

PuTTY 등에서는 연결 종류를 **Telnet**, 호스트를 **생명장치 IP**, 포트를 **23**으로 설정합니다. 동시 접속은 한 명만 가능합니다. `Telnet already connected.`가 나오면 기존 접속을 종료합니다.

접속 후 다음 명령으로 모드를 확인합니다.

```text
status
```

`mode=card-upload`이면 준비된 상태입니다. 이때 `blocked=1`은 게임 태그 처리와 릴레이 열림이 차단됐다는 정상 표시입니다. `mode=inactive`이면 서버 설정과 장치의 서버 연결을 확인합니다.

**Telnet에 `devicestate:card-upload`를 입력하는 방식은 지원하지 않습니다.** 모드는 서버에서 별도로 변경해야 합니다. `read`, `write`, `save`는 서버의 `card-upload` 모드에서만 실행됩니다.

## 2. 빠른 사용: G1P1 생명칩 만들기

카드를 리더에서 떼어 둔 뒤, Telnet에 한 줄씩 입력합니다. 아래 코드는 명령 자체이므로 따옴표나 코드 블록 표시를 붙이지 않습니다.

```text
defaults
preview G1P1
write G1P1
```

- `defaults`: 게임용 기본 설정으로 되돌립니다.
- `preview G1P1`: 생성할 URL이 `https://G1P1.p.fuzzyline.io`인지 확인합니다. 카드를 읽거나 쓰지 않습니다.
- `write G1P1`: 한 번의 기록 작업을 시작합니다.

`preview`에서 다음 항목을 확인합니다.

```text
content=https://G1P1.p.fuzzyline.io
prefix=0x04
page7=47315031
```

위 항목은 실제 출력의 일부입니다. `page7=47315031`은 게임에서 읽는 `G1P1`의 16진수 표현입니다.

`write` 뒤에 다음 안내가 나옵니다.

```text
ARMED write for 30s: remove the existing tag, then present one tag and hold it still.
Ready: present one tag
```

**`Ready: present one tag`가 나온 뒤 카드 한 장을 대고, 아래 성공 메시지가 나올 때까지 움직이지 않습니다.** 이미 카드를 대고 있었다면 먼저 완전히 떼어야 합니다. 제거 확인에는 최소 400ms 동안 두 번의 정상적인 카드 없음 감지가 필요합니다.

```text
OK WRITE verified; remove tag before another job
```

이 메시지는 같은 UID의 카드에 기록한 영역을 다시 읽어 내용이 일치함을 확인했다는 뜻입니다. 이제 카드를 떼고, 필요하면 휴대전화의 NFC 읽기로 URL도 확인합니다.

기본 설정에서는 전체 URL을 입력해도 같습니다.

```text
preview https://G1P1.p.fuzzyline.io
write https://G1P1.p.fuzzyline.io
```

다음 카드를 만들 때는 새 명령을 실행하고 다시 `Ready` 안내를 기다립니다.

```text
write G1P2
```

작업 제한 시간은 **명령 실행 시점부터 제거 대기·인식·기록·검증을 포함해 총 30초**입니다. 성공하거나 실패한 작업은 자동으로 재시도하지 않습니다. 여러 `write` 명령을 한꺼번에 붙여 넣지 않습니다.

## 3. 명령어 모음

명령어와 인수는 대소문자를 구분하며, 각 줄 끝에서 Enter를 누릅니다.

| 명령 | 설명 |
| --- | --- |
| `help` | 지원 명령어 표시 |
| `status` | 현재 모드, 게임 차단 여부, 설정, 진행 중인 작업, 마지막 결과 확인 |
| `defaults` | 현재 접속의 설정을 기본값으로 복원 |
| `format uri` | NDEF URI 레코드로 기록: URL용 |
| `format text` | NDEF UTF-8 Text 레코드로 기록: 언어 코드 `en` |
| `prefix auto` | 입력과 일치하는 지원 URI 접두어 중 가장 긴 접두어 자동 선택 |
| `prefix none` | URI 접두어 압축 없이 전체 문자열 기록 |
| `prefix http://www.` | URI 접두어 `0x01` 사용 |
| `prefix https://www.` | URI 접두어 `0x02` 사용 |
| `prefix http://` | URI 접두어 `0x03` 사용 |
| `prefix https://` | URI 접두어 `0x04` 사용 |
| `template <pattern>` | `{code}`가 정확히 한 번 들어간 템플릿 지정 |
| `layout game` | 기존 생명장치의 게임 코드 읽기 위치에 맞춰 기록 |
| `layout standard` | 일반 NDEF 기록. 게임 호환 보장 없음 |
| `preview <value>` | 생성 결과, 크기, 접두어 코드, page 7 확인. 카드 접근 없음 |
| `write <value>` | 카드 한 장 기록 및 읽기 검증 예약 |
| `read` | 카드 한 장의 UID·모델·사용자 메모리 144바이트 읽기 예약 |
| `cancel` | 대기/진행 중인 작업 중단. 이미 쓴 데이터는 복원하지 않음 |
| `save` | 현재 설정을 장치의 비휘발성 메모리에 저장 |

기본 설정은 다음과 같습니다.

```text
format uri
prefix auto
layout game
template https://{code}.p.fuzzyline.io
```

설정 변경과 `defaults`는 `save` 전까지 현재 접속에서만 유지됩니다. 재접속하면 마지막으로 저장한 설정을 불러옵니다. 기본값을 다음 접속에도 유지하려면 `defaults` 다음에 `save`를 실행합니다. 카드 기록을 위해 반드시 `save`할 필요는 없습니다.

`save`는 설정만 저장합니다. 진행 중인 작업이나 서버 모드를 저장하지 않습니다. 작업 중 설정 변경·새 작업·저장을 시도하면 `ERROR job already armed; cancel it first`가 나옵니다. 먼저 작업을 완료하거나 `cancel`합니다.

## 4. NDEF 형식과 URL 접두어 선택

### 게임용 생명칩

일반적인 생명칩은 **`format uri` + `prefix auto` + `layout game`**을 사용합니다. 게임 코드는 대문자 한 자리씩인 `G0P0`부터 `G9P9`까지입니다. `G10P1`, `g1p1`, 관리자 코드 `MMMM`은 게임용 기록값으로 지원하지 않습니다.

URI 모드에서 템플릿은 **입력 전체가 정확히 `G#P#`일 때만** 적용됩니다. 전체 URL이나 호스트 이름을 입력하면 그대로 사용합니다. 게임 중 장치는 page 7의 네 글자만 읽으며, 전체 URL을 해석하거나 웹사이트에 접속하지 않습니다.

### `https://`가 저장되는 방식

NDEF URI는 URL의 앞부분을 1바이트 코드로 줄여 저장할 수 있습니다.

| URI 접두어 코드 | 읽을 때 복원하는 문자열 |
| --- | --- |
| `0x00` | 없음: 전체 URI가 문자열에 들어 있음 |
| `0x01` | `http://www.` |
| `0x02` | `https://www.` |
| `0x03` | `http://` |
| `0x04` | `https://` |

따라서 `prefix=0x04`이고 URI 본문이 `G1P1.p.fuzzyline.io`라면 전체 URL은 **`https://G1P1.p.fuzzyline.io`**입니다. 덤프에 `https://`의 글자가 그대로 보이지 않아도 접두어 코드에 포함되어 있습니다.

`https://www.`도 지원합니다. 다만 `https://G1P1.p.fuzzyline.io`와 `https://www.G1P1.p.fuzzyline.io`는 서로 다른 주소입니다. 생명칩 기본 URL에는 `www.`를 추가하지 않습니다.

### 일반 웹 링크

게임용 코드가 없는 웹 링크는 `layout standard`로 기록합니다.

```text
layout standard
format uri
prefix https://www.
preview example.com
write example.com
```

위 예시는 `https://www.example.com`을 만듭니다. 수동 접두어를 선택한 상태에서 전체 URL을 입력한다면 해당 접두어와 일치해야 합니다. 예를 들어 `prefix https://www.`에 `https://example.com`을 입력하면 `prefix_mismatch`로 거절합니다. 호스트 이름의 `www.`는 중복해서 붙이지 않습니다.

`prefix auto` 또는 `prefix none`은 빠진 `https://`를 자동으로 붙이지 않습니다. 웹 링크가 목적이라면 전체 HTTP(S) URL을 입력합니다. `prefix none`으로 압축하지 않은 HTTP(S) URL을 기록할 때도 `layout standard`가 필요합니다.

### 일반 텍스트

```text
format text
layout standard
template {code}
preview TEST001
write TEST001
```

UTF-8 Text 레코드에 `TEST001`을 기록합니다. Text 모드에서는 모든 입력에 템플릿이 적용되므로, 입력 그대로 저장하려면 위처럼 `template {code}`를 사용합니다. Text 모드에서는 URI 접두어가 사용되지 않습니다.

**Text 레코드는 `layout game`과 함께 사용할 수 없습니다.** 일반 NDEF 카드 제작 후 게임용 생명칩으로 돌아갈 때는 `defaults`로 설정을 복원합니다.

## 5. 카드 읽기 및 결과 확인

```text
read
```

쓰기와 동일하게 먼저 카드를 떼고 `Ready: present one tag`를 기다린 뒤 카드 한 장을 고정합니다. NT3H1101 예시는 다음과 같습니다.

```text
UID=04086F52787480
VERSION=0004040502011303
MODEL NT3H1101 (NTAG I2C 1K)
page4=...
page8=...
...
page36=...
OK READ complete: 144 user-memory bytes
```

`page4`, `page8` 등의 각 행은 시작 페이지부터 16바이트, 즉 네 페이지의 데이터입니다. `read`는 원시 16진수 덤프를 출력하며 URL을 자동으로 해석해 표시하지 않습니다. `preview`는 쓰기 전 설정으로 만들 결과만 보여 주므로, 실제 카드 검증에는 `OK WRITE verified`와 기록 후 읽기를 사용합니다.

## 6. 오류가 났을 때

| 표시 | 의미와 조치 |
| --- | --- |
| `OK WRITE verified` | 기록한 내용의 읽기 검증 완료. 카드를 제거하고 다음 작업 진행 |
| `ERROR ...` | 쓰기 시도 전 오류 또는 읽기 오류. 원인을 해결한 뒤 새 명령으로 재시도 |
| `UNKNOWN ...` | 쓰기를 한 번 이상 시도한 뒤 실패. 카드 일부가 바뀌었을 수 있음. 성공으로 취급하지 말고 `read`로 먼저 확인 |
| `server device_state must be card-upload` | 서버에서 해당 장치의 모드를 변경하고 `status` 확인 |
| `job timed out; no automatic retry` | 총 30초 초과. 카드를 제거하고 새 작업 실행. `UNKNOWN`이면 먼저 읽기 확인 |
| `different UID; no retry` | 도중에 다른 카드 감지. 한 장만 두고 다시 진행 |
| `unsupported tag` | 지원 모델/버전 확인. 구형 펌웨어라면 v68 지원 여부 확인 |
| `locked, protected or mirrored tag refused` | 잠금·보호·미러링 상태로 기록 거부. 이 업로더에는 잠금 해제 기능 없음 |
| `NTAG I2C mirrored, pass-through, busy or RF write disabled` | I2C 카드의 모드·점유·RF 쓰기 조건 확인. 연결된 I2C 호스트는 작업 중 유휴 상태 유지 |
| `PN532 phase=... fault=...` | 카드/리더 통신 실패. 카드 위치를 안정적으로 고정하고 로그 보관. 쓰기 시도 후라면 내용부터 확인 |

연결 종료, `cancel`, 서버 모드 변경, 카드 이탈, 전원 차단으로 작업이 중단될 수 있습니다. **기록은 여러 페이지에 걸쳐 수행되므로 중간 실패 때 이전 내용을 자동으로 복구하지 않습니다.** `UNKNOWN` 이후에는 카드 제거 → `read` → `Ready` 확인 → 같은 카드 제시 순서로 상태를 확인하고, 필요할 때 새 `write`를 실행합니다. 자동 재시도나 자동 다음 카드 기록은 없습니다.

## 7. 지원 카드 및 저장 범위

- 지원 모델: **NXP NTAG213 / NTAG215 / NTAG216**, **NT3H1101 원본 NTAG I2C 1K**.
- NT3H1101 지원 버전 응답: `00 04 04 05 02 01 13 03`. NT3H1201(2K), NTAG I2C Plus 및 다른 모델/호환되지 않는 버전은 지원하지 않습니다.
- 기록 가능 범위는 page 4~39의 **최대 144바이트**입니다. 큰 용량의 카드를 사용해도 업로더의 한도는 같습니다.
- 입력 및 생성 콘텐츠는 최대 128 UTF-8 바이트, 템플릿은 최대 96바이트이며 NDEF 헤더와 정렬을 포함한 전체 기록 이미지도 144바이트 안에 들어야 합니다. 한글은 글자 수와 바이트 수가 다릅니다.
- UID, CC, 잠금 비트, 암호 및 설정 페이지는 쓰지 않습니다. 쓰기 전에 잠금·보호·기존 메모리 구조 등을 검사하며, 지원하지 않는 사용자 정의 구조는 거절합니다.
- NT3H1101은 추가 상태 확인 때문에 다른 지원 칩보다 작업이 오래 걸릴 수 있습니다. 연결된 I2C 호스트는 카드 작업 중 유휴 상태여야 합니다.

## 8. 업로드 모드 종료 후 게임 복귀

1. 진행 중인 작업을 완료합니다. 중단해야 한다면 `cancel`하고, `UNKNOWN`이면 카드 내용을 확인합니다.
2. 서버에서 대상 장치의 `device_state`를 정상 상태인 `activate`, `tagger`, `ready`, `setting` 중 운영에 맞는 값으로 변경합니다. 일반적인 게임 복귀에는 `activate`를 사용합니다.
3. 카드를 리더에서 완전히 제거하고 장치가 제거를 확인할 때까지 기다립니다.
4. Telnet의 `status`에서 **`mode=inactive blocked=0`**을 확인합니다. 게임 진행에는 서버의 `game_state`도 운영 상황에 맞아야 합니다.
5. 게임용 카드를 새로 태그해 사용합니다. 필요하면 Telnet 연결을 종료합니다.

Telnet을 끊는 것만으로 서버의 `card-upload` 모드가 해제되지는 않습니다. 정상 서버 상태와 카드 제거가 모두 확인될 때까지 게임과 릴레이 열림은 계속 차단됩니다. 작업 중 차단된 이전 `open` 요청이나 OTA 요청은 자동 재실행되지 않으므로, 정상 복귀 후 필요하면 새로 요청합니다.

## 참고

문서 기준: 2026-09-25, 펌웨어 v68. 최신 내용은 [GitHub 사용 가이드](https://github.com/Fuzzyline-HAS2/New_HAS1/blob/main/devices/HAS1_revival_machine/CARD_UPLOAD_KO.md)에서 확인합니다.

[Notion 사용 가이드](https://app.notion.com/p/3e60bd3810bf81f99d54d2251c483baf)에서도 같은 사용 절차를 확인할 수 있습니다.

이 기능은 기존 LAN Telnet 콘솔을 사용하며 별도의 사용자 로그인은 없습니다. 서버의 `card-upload` 모드가 기록 허용 조건입니다. 작업할 때만 해당 모드를 활성화합니다.

2026-09-25 Academy AR에서 NT3H1101 읽기·기록 및 정상 동작을 사용자 테스트로 확인했습니다. 이 결과는 해당 장치·카드 조합 기준이며 휴대전화별 인식 여부는 별도 확인 대상입니다.

- [영문 기술 설명 및 프로토콜 참고 자료](CARD_UPLOAD.md)
- [생명장치 README](README.md)
- [v68 릴리즈](https://github.com/Fuzzyline-HAS2/New_HAS1/releases/tag/HAS1_revival_machine)
