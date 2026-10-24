# HAS1 blackbox Windows collector

글러브와 발전기의 별도 `HAS1BlackBox` 로그를 수집하며, 선택적으로 게임 서버·Omada 기록과 사건 알림을 함께 처리합니다. 게임 HTTP와 기존 OTA, 공용 Wi-Fi 라이브러리는 변경하지 않고 `windows-telnet-logger`에는 연결하지 않습니다.

1호점 최초 설치는 [FIRST-INSTALL.md](FIRST-INSTALL.md), 게임 상황에 따른 사건 판단은 [서버 로그·사건 수집 활성화](#서버-로그사건-수집-활성화), Slack·Notion 연결은 [NOTIFICATIONS.md](NOTIFICATIONS.md)를 순서대로 참고하세요.

```
장치 flash → MQTT QoS 1 → Mosquitto → bounded queue → SQLite COMMIT → ACK1
```

**MQTT PUBACK은 저장 완료가 아닙니다.** 수집기가 SQLite `WAL` +
`synchronous=FULL` 트랜잭션을 커밋한 다음 보내는 `ACK1`만 정상 전송 완료로
인정합니다. 다만 용량이 가득 차면 미수집된 가장 오래된 섹터도 순환 덮어쓰며,
가능한 한 LOST로 유실 범위를 남깁니다. ACK가 유실되면 남아 있는 기록을 다시 보내고, `(mac, ring, seq)` 중복은 한 번만
저장합니다. 같은 식별자에 다른 데이터가 들어오면 전체 배치를 롤백하고 ACK를
보내지 않습니다. 시퀀스는 재부팅 시 예약 구간 때문에 건너뛸 수 있습니다.

## Windows 설치

1. Python **3.11 이상**(Windows `py` launcher 포함)을 설치합니다.
2. [공식 Mosquitto Windows 배포본](https://mosquitto.org/download/)을 설치합니다.
   수집기는 브로커 서비스를 자동 설치하거나 방화벽을 변경하지 않습니다.
3. 이 폴더를 예를 들어 `C:\HAS1\windows-blackbox-logger`에 복사하고 `setup.cmd`를
   실행합니다. 전용 `.venv`에 고정 버전 `paho-mqtt==2.1.0`만 설치합니다.
4. `mosquitto/mosquitto.example.conf`를 `mosquitto/mosquitto.conf`로 복사해 실제 PC
   LAN 주소와 절대 경로를 설정합니다. `acl.example`을 `acl`로 복사합니다.
5. 명령 프롬프트에서 암호 파일을 만듭니다. 첫 명령의 `-c`는 **처음 한 번만**
   사용합니다. 이후 장치 추가 때 `-c`를 쓰면 기존 계정이 지워집니다.

```bat
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -c mosquitto\passwords collector
"C:\Program Files\mosquitto\mosquitto_passwd.exe" mosquitto\passwords AABBCCDDEEFF
"C:\Program Files\mosquitto\mosquitto.exe" -c mosquitto\mosquitto.conf -v
```

장치별 사용자는 **대문자 MAC 12자리**입니다. ACL은 사용자별 자기 batch 발행과
자기 ack 구독만 허용합니다. 장치에는 해당 계정/암호를 설정합니다. 로그 수집기
계정은 `collector`입니다. 현재 장치 어댑터는 `mqtt://IPv4:port`만 지원하며 TLS 연결은 지원하지 않습니다.
평문 MQTT와 암호는 격리된 신뢰 LAN에서만 사용합니다. 외부망 적용은 장치 TLS
지원과 인증서 검증을 추가한 뒤 별도 검증이 필요합니다. 암호와 실제 설정 파일은
Git에서 제외됩니다.

6. `config.json`의 host를 브로커 listener 주소로 맞춥니다. 샘플 `127.0.0.1`은
   브로커가 loopback에도 listen할 때만 유효합니다. PC와 장치는 동일 브로커를
   사용해야 하며 방화벽 허용 범위는 해당 장치 LAN으로 제한합니다.
7. 새 명령 프롬프트에서 수집기를 시작합니다.

```bat
set HAS1_BLACKBOX_PASSWORD=수집기계정암호
start.cmd
```

암호를 저장소에 커밋하지 않습니다. 위 실행은 콘솔 방식이며 중지는 `Ctrl+C`입니다.
로그인하지 않아도 자동 수집하려면 아래 Windows 서비스 등록을 사용합니다.
재실행해도 SQLite 데이터는 유지됩니다. 브로커/PC가 없는 동안에는
장치 flash 보존 정책이 적용됩니다. flash가 가득 차면 무한히 보존되는 것은 아니며
장치 로그 드롭 카운터를 확인해야 합니다.

## Windows 서비스 등록

서비스 등록은 **수집기를 백그라운드에서 자동 실행**하는 기능입니다. Slack·Notion 알림은 별도 설정으로 활성화하며 기본적으로 꺼져 있습니다. 설정 GUI는 제공하지 않습니다. Claude/Codex 실행은 필요하지 않습니다.
Mosquitto 설치·서비스 등록·방화벽 설정도 이 도구가 변경하지 않습니다.

지원 대상은 **64비트 Windows 10/11, .NET Framework 4.8 이상, 모든 사용자용
64비트 Python 3.11 이상**입니다. Python은 `C:\Program Files\Python311\python.exe`
같은 보호된 위치에 설치해야 합니다. 개인 계정의 AppData/WindowsApps Python은
로그아웃 후 LocalService 계정이 사용할 수 없어 설치 단계에서 거부합니다.
설치 후 Python 기본 설치 경로를 이동하거나 제거하면 서비스도 실행되지 않습니다.

1. 기존 `start.cmd` 수집기를 `Ctrl+C`로 종료합니다. 동일 MQTT `client_id`로 구형
   수집기나 다른 PC의 수집기를 동시에 실행하지 마십시오.
2. `config.json`의 브로커 주소·포트·사용자·client_id를 실제 운영 값으로 설정합니다.
3. **관리자 권한 명령 프롬프트**에서 다음을 실행합니다.

```bat
service.cmd install -PythonExe "C:\Program Files\Python311\python.exe"
service.cmd start
service.cmd status
```

설치 중 MQTT 수집기 암호를 숨김 입력으로 받습니다. 암호를 명령행/XML에 넣거나
`HAS1_BLACKBOX_PASSWORD`를 시스템 환경 변수에 저장할 필요가 없습니다. 서비스는
**LocalService** 계정으로 실행되며 암호는 Windows **machine DPAPI**로 암호화하고
Administrators/SYSTEM/LocalService만 접근하는 파일에 보관합니다. machine DPAPI는
사용자별 접근 제한을 대신하지 않으므로 이 파일의 ACL을 넓히지 마십시오. 다른 PC로
암호 파일만 복사해도 복호화할 수 없습니다.

`install`은 **등록만 하고 즉시 시작하지 않습니다**. `start` 후 로그인 없이 실행하며,
다음 Windows 부팅부터 지연 자동 시작합니다. 처음 설치는 `config.json`을 가져오고,
기존 서비스 설정이 남아 있으면 재설치 시 그대로 사용합니다.

| 위치 | 용도 |
|---|---|
| `%ProgramFiles%\HAS1 BlackBox` | 보호된 코드·전용 `.venv`·WinSW 서비스 래퍼 |
| `%ProgramData%\HAS1BlackBox\config.json` | 실제 서비스 설정(관리자 수정) |
| `%ProgramData%\HAS1BlackBox\mqtt-password.dpapi` | 암호화된 MQTT 암호 |
| `%ProgramData%\HAS1BlackBox\data\blackbox.sqlite3` | 서비스가 수집한 원본 DB |
| `%ProgramData%\HAS1BlackBox\data\collector.log` | 회전 수집기 로그 |
| `%ProgramData%\HAS1BlackBox\data\service-logs` | WinSW wrapper 로그와 회전 stdout/stderr 로그 |

**기존 콘솔 DB를 자동 이동하거나 복사하지 않습니다.** 서비스는 위 ProgramData DB를
사용합니다. 기존 이력은 원래 DB에 남습니다. 이전 이력과 합치는 작업은 별도이며,
실행 중 SQLite 본체만 복사하면 WAL의 미반영 기록이 누락될 수 있으므로 피하십시오.
서비스 `database` 경로는 위 고정 위치를 유지해야 합니다. `tls_ca`가 있으면 첫 설치 때
서비스 설정 폴더로 복사합니다. 이후 인증서 교체는 서비스를 중지한 후 해당 파일을
관리자로 교체하십시오.

첫 설치는 **WinSW 2.12.0 `WinSW.NET461.exe`** 고정 URL에서 다운로드하고 SHA256을
검증한 뒤 사용합니다. 고정된 해시는
`b5066b7bbdfba1293e5d15cda3caaea88fbeab35bd5b38c41c913d492aadfc4f`입니다.
프로젝트가 배포한 별도 서명 해시가 아니라 공식 배포 파일을 직접 계산해 고정한 값입니다.
Windows에 설치된 .NET Framework를 사용하며 오래된 .NET Core 런타임을 동봉한
다른 WinSW 실행 파일로 자동 대체하지 않습니다. 이미 내려받은 동일 파일도 지원합니다.

```bat
service.cmd install -PythonExe "C:\Program Files\Python311\python.exe" -WinSWBinary "D:\Install\WinSW.NET461.exe"
```

이 옵션은 **WinSW 다운로드만 생략**합니다. 전용 `.venv`의 고정 `paho-mqtt` 설치에는
pip 저장소 연결이 필요합니다. 설치 실패 시 원인 수정 후 다시 실행할 수 있으며,
설정·DB는 삭제하지 않습니다. 재등록은 관리자의 명시적 `uninstall` 후 수행합니다.

### 중지·재시작·설정 변경·제거

```bat
service.cmd stop
service.cmd restart
service.cmd status
service.cmd uninstall
```

- `stop`은 Python에 Ctrl+C를 전달해 신규 접수를 멈추고 이미 받은 배치를 저장한 뒤
  종료합니다. 정상 중지 로그는 `Collector stopped cleanly after draining accepted batches`입니다.
- 디스크/라이브러리가 멈춰 정상 종료하지 못하면 WinSW가 **프로세스마다 최대 30초
  대기 후 강제 종료**합니다. Windows venv 실행기가 추가 프로세스를 만들 수 있으므로
  전체 종료가 정확히 30초 이내라는 보장은 아닙니다. 부모 프로세스부터 종료 신호를 전달해 Python이 Ctrl+C를 받고 저장 작업을 마칠 수 있게 하며, 관리 명령은
  SCM 중지 상태를 최대 90초 확인한 뒤 성공 또는 오류를 반환합니다.
  그 시점의 미커밋 배치는 ACK하지 않습니다. 장치 플래시에 남아 있는 기록은 재전송으로
  복구하지만, 순환 덮어쓰기로 사라진 기록까지 보장하지는 않습니다.
- 비정상 프로세스 종료는 10초, 30초, 이후 60초 간격으로 재시작합니다. 수동 `stop`은
  재시작하지 않습니다. 계속 실패하는 경우 로그를 보고 `stop`으로 반복을 멈춥니다.
- `collector.log`와 child stdout/stderr는 회전하지만 WinSW 2.12의 `.wrapper.log`는
  자동 회전하지 않습니다. 디스크 공간을 점검하고 필요하면 서비스를 중지한 뒤
  해당 wrapper 로그를 별도 보관/정리하십시오.
- 브로커 일시 중단은 수집기 내부의 5~60초 재접속을 사용합니다. 서비스 Running 상태는
  MQTT 연결/로그 수신 성공을 의미하지 않으므로 `collector.log`도 확인하십시오.
- 브로커 주소/사용자 변경: `stop` → ProgramData의 `config.json` 편집 → `start`.
  실행 폴더의 원본 `config.json`을 바꿔도 설치된 서비스에는 반영되지 않습니다.
- 암호 변경: `stop` → `service.cmd credential` → 숨김 입력 → `start`.
- `uninstall`은 중지 후 SCM 등록만 제거합니다. **설정·암호·DB·로그·프로그램 폴더는
  보존**합니다. 파일 삭제는 백업을 확인한 운영자가 별도로 수행합니다.
- Windows에서는 동일 브로커 주소/포트/client_id의 서비스·콘솔 동시 실행을 전역 mutex로
  거부하고, 같은 DB의 동시 실행도 OS 파일 잠금으로 막습니다. 호스트 별칭(`localhost`와
  `127.0.0.1`), 다른 PC, 구버전 수집기는 같은 대상으로 인식할 수 없으므로 운영상 중복
  실행을 피하십시오. 프로세스 강제 종료 후 잠금은 OS가 자동 해제합니다.

LocalService는 프로그램·설정·암호를 읽고 `data`에만 쓸 수 있습니다. 이 권한 구분을
유지해야 하며 Program Files 대신 사용자 다운로드 폴더에서 서비스 코드를 실행하도록
XML을 바꾸지 마십시오. `--credential-file`을 지정한 설정 검사는 암호 복호화도 검증하며,
설치·시작 전에 실패하면 서비스를 시작하지 않습니다.

서비스 자체/PC가 꺼진 상황을 외부에 알리는 감시 기능은 포함하지 않습니다. Windows
서비스 관리자에서 상태와 시작 유형을 확인할 수 있습니다.

근거: [WinSW 2.12 설정](https://github.com/winsw/winsw/blob/v2.12.0/doc/xmlConfigFile.md),
[WinSW 종료 구현](https://github.com/winsw/winsw/blob/v2.12.0/src/WinSW/WrapperService.cs),
[Microsoft DPAPI](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata),
[Windows 커널 객체 이름 공간](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces).

## 운영과 조회

- `data/blackbox.sqlite3`: 원본 64바이트 레코드와 장치/수신 UTC. 장치 uptime은
  벽시계 시간이 아니므로 수신 시각을 사고 발생 시각으로 단정하지 않습니다.
- `data/collector.log`: 최대 1 MiB × 4개 회전 로그. 30초마다 커밋/ACK/오류/큐 통계.
- 수집기 큐 64개, 배치 최대 16개, payload 최대 4096바이트. MQTT callback에서
  DB/fsync를 하지 않습니다. 큐가 가득 차거나 저장이 실패하면 ACK하지 않습니다.
- SQLite 파일은 **로컬 디스크**에 둡니다. 네트워크 공유/동기화 폴더는 피합니다.
  FULL 동기화도 저장장치가 flush 요청을 지키는 범위에서만 내구성을 보장합니다.
- DB 크기는 자동 제한하지 않습니다. 확인되지 않은 로그를 자동 삭제하지 않으므로
  디스크 잔여 공간을 운영 점검해야 합니다. 디스크가 가득 차면 오류를 기록하고
  ACK를 중단하며 공간 확보 후 장치 재전송을 받을 수 있습니다.
- 손상된 DB는 자동 초기화하지 않습니다. 수집기를 중지하고 DB와 `-wal`, `-shm`
  파일을 함께 보존한 뒤 별도 복구합니다. 실행 중 DB 파일만 복사하지 마십시오.

```bat
.venv\Scripts\python.exe export.py data\blackbox.sqlite3 --mac AABBCCDDEEFF --limit 10000 > logs.jsonl
.venv\Scripts\python.exe decode_flash.py glove-spiffs-128k.bin > offline.jsonl
```

`export.py`는 읽기 전용으로 최대 100만 행을 JSONL로 내보냅니다. 숫자 이벤트 타입,
이름, 4개 값, text, 원본 hex를 포함합니다. 각 값의 뜻은 펌웨어의 기록 지점과
`libraries/HAS1BlackBox`와 장치별 `BLACKBOX.md`를 참고합니다. 미측정 RSSI는
`INT32_MIN`, 장치 전압/ADC 등 일부 필드는 `-1`이며 필드별 규칙을 따라 해석합니다.

`decode_flash.py`는 별도로 확보한 **128 KiB partition 파일만** 읽습니다. 장치
연결/리셋/erase/write 기능이 없습니다. 앞쪽 4 KiB 섹터 2개의 metadata journal
(64바이트 헤더 최대 128개)을 모두 스캔하고 CRC가 유효한 가장 높은 generation을
선택합니다. 중간에 잘린 헤더는 건너뛰되 원본 hex를 출력하며, 섹터 순환 후 새 헤더가
낮은 주소에 있어도 찾습니다. 기존 섹터당 헤더 1개 형식도 읽을 수 있습니다. 유효한
metadata가 없거나 서로 다른 ring 또는 같은 generation의 서로 다른 헤더가 있으면
자동 복구하지 않고 중단합니다. 유효 레코드·손상 슬롯·원본 hex를 출력하며 원본 dump는
변경하지 않습니다. 원본 dump를 유지하십시오. flash 추출 주소는 실제
장치 partition table에서 확인해야 하므로 이 도구는 고정 주소로 읽지 않습니다.

## Wire v1

- Publish: `has1/blackbox/v1/<MAC12uppercase>/batch`, QoS 1, retain=false.
- Header: `BB1 <iotglove|HAS1_generator> <MAC12> <ring16lowerhex> <batch8lowerhex>\n`.
- 그 뒤 1~16줄, 각각 little-endian 64바이트 레코드의 128자리 hex + `\n`.
- 레코드: B7/type/예약0 2바이트, seq/boot/uptime 각각 u32,
  i32 값 4개, text 28바이트, 앞 60바이트 IEEE CRC32 u32.
- seq는 0이 아니며 배치 안에서 엄격히 증가합니다. 중복 배치는 허용합니다.
- App ACK topic: `has1/blackbox/v1/<MAC12>/ack`;
  payload: `ACK1 <ring16lowerhex> <batch8lowerhex> <lastSeqDecimal>\n`.
- 앱 ACK는 QoS 1, retain=false. 브로커 persistence/세션 큐는 저장 확정 수단이
  아닙니다. 장치 재시작 후에도 미확인 flash 로그를 재전송합니다.

## 검증

```bat
.venv\Scripts\python.exe -m unittest discover -s tests -v
```

CRC/길이/identity/순서 검증, real SQLite 중복·충돌·FULL 오류·읽기 전용 오류,
COMMIT 전/후 프로세스 강제 종료, 손상 DB 거부, 큐 포화, ACK 발행 실패, JSONL 출력,
raw flash decoder를 검증합니다. `MOSQUITTO_BIN` 환경 변수 또는 PATH에 브로커가
있으면 실제 localhost MQTT + collector 프로세스 + DB ACK + 브로커 재시작 시험도
실행합니다. 테스트 브로커는 임의 loopback 포트이며 서비스 설치는 하지 않습니다.

```bat
set MOSQUITTO_BIN=C:\Program Files\mosquitto\mosquitto.exe
.venv\Scripts\python.exe -m unittest discover -s tests -v
```

CI는 Windows/Linux Python 3.11에서 단위 테스트를 실행하고 Linux에 Mosquitto를
설치해 실제 브로커 테스트를 실행합니다. Windows 브로커 설치·실제 장치 부하·
네트워크 단절·배터리 분리 시험은 별도 현장 검증 대상입니다.

이 구현은 이전 계획의 C#5 수집기 대신 Python+paho+sqlite3를 사용합니다.
별도 DB 서버/수동 journal 복구 코드를 줄이고 Windows와 개발 환경에서 동일한
저장·장애 복구 코드를 시험하기 위한 선택입니다.

근거: [Paho client API](https://eclipse.dev/paho/files/paho.mqtt.python/html/client.html),
[SQLite synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous),
[Mosquitto 설정](https://mosquitto.org/man/mosquitto-conf-5.html).

서비스 관련 단위 테스트는 잠금 경쟁/프로세스 종료 후 잠금 해제, 종료 중 수락된 배치의
COMMIT→ACK 순서, XML 설정, Windows DPAPI 왕복을 확인합니다. 일반 테스트는 실제
서비스를 등록하지 않습니다. 별도 Windows CI 작업은 일회용 관리자 runner에서
`tests/windows_service_smoke.ps1`을 실행해 LocalService 등록·실행·실제 DB 생성·
암호 복호화·다른 콘솔 중복 거부·강제 종료 후 재시작·정상 중지·
제거/재설치 시 데이터 보존을 확인합니다. **이 smoke script를 운영 PC에서 실행하지
마십시오**. 기존 HAS1 설치가 있으면 중단하며 임시 시험 설치는 끝난 뒤 삭제합니다.
현재 macOS 검증 결과와 Windows에서 추가 확인할 항목은
`../../docs/blackbox/WINDOWS_SERVICE_VALIDATION.md`를 참고하십시오.

## 1호점 Omada·게임 서버 로그 자동 수집

`config.store1.example.json`은 **두 추가 입력 모두 비활성화**한 현장 설정 예시입니다.
기존 MQTT 수집은 그대로 동작합니다. 확인된 현장 정보는 Windows 수집기·게임 서버
`172.30.1.43`, Omada 컨트롤러 `172.30.1.1`, 컨트롤러 6.2.14.30의 사이트
`HAS1`(사이트 ID `67fe12548e0afd6f7b2d9d6f`), 시간대 Asia/Seoul입니다.
수집기 내부 사이트 식별자는 `store-1`입니다. 로그인 암호는 필요하지 않습니다.

1. 콘솔 사용 시 예시를 `config.json`으로 복사하고 MQTT 설정을 맞춥니다.
   서비스가 이미 설치되어 있으면 먼저 중지하고
   `C:\ProgramData\HAS1BlackBox\config.json`의 `telemetry` 항목을 편집합니다.
   이때 DB 경로는 `C:/ProgramData/HAS1BlackBox/data/telemetry.sqlite3`로 설정합니다.
2. `telemetry.omada.enabled`를 `true`로 설정합니다. 수신 주소
   `172.30.1.43:5514/UDP`, 허용 송신자 `172.30.1.1`, 예상 사이트 이름 `HAS1`을
   확인합니다. **Windows 수신기 준비 후** Omada의 해당 사이트 Remote Logging
   목적지를 이 주소로 설정합니다. Windows 방화벽에서 해당 송신자의 UDP 5514만
   허용하는 작업은 별도로 필요하며 이 프로그램이 자동으로 바꾸지 않습니다.
3. 게임 서버의 관측 로그 출력을 활성화하고 실제 로그 폴더를
   `telemetry.server_jsonl.directory`에 넣은 뒤 `enabled=true`로 바꿉니다.
   예시 경로는 `C:/ProgramData/HAS1BlackBox/server-telemetry`입니다. 서버의 실제
   출력 경로와 일치하는지 확인해야 합니다. 서버 설정과 `site_id=store-1`, `source_id=store-1-game-server`가
   일치해야 합니다. 같은 PC 파일을 읽으므로 별도 네트워크 포트가 필요 없습니다.
4. 서비스 계정 `LOCAL SERVICE`에 **서버 로그 폴더 읽기 권한**을 부여합니다.
   서버는 파일 생성·쓰기 권한, 수집기는 읽기 권한만 필요합니다. 계정 권한이나
   게임 서버 배포 경로는 이 설치 도구가 추측해서 수정하지 않습니다.
5. `collector.py --config config.json --check-config`로 구조를 검증하고 실행합니다.
   이 명령은 실제 bind·방화벽 통과·서버 파일 접근을 확인하지 않으므로, 실행 후
   `collector.log`의 `Telemetry counters`와 실제 `telemetry.sqlite3` 기록도 확인합니다.

서비스 첫 설치는 별도 telemetry DB를
`C:\ProgramData\HAS1BlackBox\data\telemetry.sqlite3`로 배치합니다. 재설치 시 기존
중첩 설정을 보존하며, 서비스 제거 후에도 이 DB는 지우지 않습니다. 로그 폴더는
게임 서버가 관리하며 수집기는 수정하거나 삭제하지 않습니다.

추가 입력은 MQTT 저장/ACK 작업과 별도 스레드·DB 연결에서 실행됩니다. Omada 저장
실패/큐 포화는 손실 카운터에 남고, 서버 JSONL DB 저장 실패는 해당 줄을 보관해 다음
주기에 재시도합니다. 시작 시 UDP bind 실패도 기존 MQTT 수집을 중단하지 않습니다.
Windows 부팅 시 아직 수신 IP가 준비되지 않아 bind에 실패하거나 수신 스레드가
종료되면 5~30초 간격으로 재시도합니다. 기존 소비 작업이 끝나기 전에는 새 수신기를
겹쳐 실행하지 않습니다. 30초마다 기록하는 카운터로 `site_mismatch`, `overflow`, `callback_errors`,
`storage_errors`, `invalid_rows`, `directory_errors`, `file_errors`를 확인하십시오.

서버 로그는 `server-*.jsonl` 중 이름 순 최신 8개, 파일당 최대 20 MiB를 읽습니다.
서버 작성기는 16 MiB 단위 고유 파일을 최대 8개 보관합니다. 한 주기마다 최대
256 KiB/128줄을 처리하며 완성되지 않은 줄은 기다립니다. 읽을 데이터가 있으면
1 ms 양보 후 즉시 다음 주기를 처리하고, 없거나 DB 저장에 실패하면 설정된
간격(기본 0.5초)을 기다립니다. 여러 파일은 시작 순서를 번갈아 오래된 재처리가
최신 파일을 계속 막지 않도록 합니다. 실제 처리량은 PC·디스크 성능에 의존합니다. 수집기 재시작 시 보관된
파일을 다시 읽고 이벤트 ID로 SQLite 중복 저장을 막습니다. **서버의 8개 파일이
순환 삭제되기 전에 읽지 못한 로그는 복원할 수 없습니다.** 수집기 중단/DB 장애가
길어지면 서버 파일을 별도로 보존해야 합니다. JSONL 파일을 수동 덮어쓰기·재사용하지
말고 서버의 고유 파일 회전 방식을 유지하십시오.

Omada의 명시적인 `[site:...]`가 `HAS1`과 다르면 저장하지 않고 `site_mismatch`를
증가시킵니다. 사이트 라벨이 없는 로그는 전용 목적지·허용 송신자 설정에 의존하므로,
다른 사이트를 동일 수신 주소로 보내면 안 됩니다. 원본·시간 품질·UDP 손실 한계는
[OMADA.md](OMADA.md), 장치·서버·Omada 시간 정렬과 분석은 [TIMELINE.md](TIMELINE.md)를
참고하십시오. 장치 uptime에는 자동 UTC 기준점을 만들지 않으며, 실측 기준점이
없으면 장치 기록은 시간 미정렬 상태로 남습니다.

`telemetry.sqlite3`는 **자동 삭제·보관 기간 제한이 없습니다.** 모든 요청을 기록하면
빠르게 커질 수 있으므로 용량을 주기적으로 확인하고, 보관/내보내기 정책을 별도로
운영해야 합니다. `telemetry.min_free_bytes` 기본값은 1 GiB입니다. 남은 디스크
공간을 최대 1초 간격으로 확인하여 기준 아래에서는 추가 telemetry 저장을 보류하고
`disk_reserve_blocks`를 기록합니다. 서버 파일은 다음에 재시도하지만 Omada UDP
기록은 해당 시점에 유실될 수 있습니다. 기존 MQTT 저장은 이 제한과 독립적입니다.
이 여유 공간 검사는 다른 프로세스의 디스크 소비를 막는 할당량이 아니며, 기존
로그를 자동 삭제하지 않습니다. 서버 JSONL 8개 순환 공간도 별도로 필요합니다.

### 10/24 장애 증거 보완

자동 장치 DB 반영, 선택적 MQTT 시간 기준점, PHP loopback UDP 수집, 영속 수집 상태,
원본 조회 페이지·시각 필드·명명된 값, 명시적 컬럼 매핑의 Omada CSV 가져오기는
[EVIDENCE.md](EVIDENCE.md)를 참조한다. 새 선택 기능은 기본값이 꺼져 있으며 현장 활성화가 필요하다.


## 서버 로그·사건 수집 활성화

새 사건 기능은 최신 서버 코드와 Windows 수집기를 모두 적용해야 사용할 수 있습니다. 장치 OTA를 추가로 변경할 필요는 없습니다. 설치 예시의 `incidents`와 `notifications`는 모두 꺼져 있으므로, 서버 기록 수집부터 확인하고 알림을 활성화합니다.

업데이트 전에 수집 서비스를 정상 중지하고 `data` 폴더를 백업합니다. 통합 원본 `telemetry.sqlite3`는 새 수집기에서 스키마 v3로 자동 이관되며 구형 수집기는 이 DB를 읽지 못합니다. 구형 프로그램으로 되돌릴 때는 중지 상태에서 업데이트 전 백업도 함께 복원해야 하며, 백업 이후 수집된 기록은 별도로 보존합니다. 장치 ACK 저장용 `blackbox.sqlite3`의 스키마는 이번에 바꾸지 않습니다.

1. 게임 서버의 진단 출력을 켭니다. 서버 저장소의 `docs/BLACKBOX-TELEMETRY.md`에 따라 환경 변수 `HAS1_TELEMETRY_ENABLED=1`, `HAS1_TELEMETRY_SITE_ID=store-1`, `HAS1_TELEMETRY_SOURCE_ID=store-1-game-server`, `HAS1_TELEMETRY_EXPECTED_THEME=실제_서버_테마`, `HAS1_TELEMETRY_DIR=실제_절대_로그_폴더`를 설정합니다. 실제 테마를 추측하지 말고 실행 중인 서버와 맞춥니다. 환경 변경은 서버 재시작이 필요하므로 게임이 없는 시간에 적용합니다.
2. `server-*.jsonl` 파일이 생성되고 `game_monitor_snapshot`이 기록되는지 확인합니다. LocalService 계정에 이 폴더를 읽을 권한이 있어야 합니다.
3. 수집 서비스를 중지하고 `%ProgramData%\HAS1BlackBox\config.json`의 기존 `telemetry` 안에 다음 설정을 병합합니다. 전체 파일을 아래 내용으로 덮어쓰지 않습니다. 경로 예시는 실제 서버 출력 위치와 맞춥니다.

```json
{
  "server_jsonl": {
    "enabled": true,
    "directory": "C:/ProgramData/HAS1BlackBox/server-telemetry",
    "source_id": "store-1-game-server",
    "poll_seconds": 0.5
  },
  "incidents": {
    "enabled": true,
    "database": "C:/ProgramData/HAS1BlackBox/data/incidents.sqlite3",
    "notifications": {"enabled": false}
  }
}
```

서비스 코드 기본 위치는 `%ProgramFiles%\HAS1 BlackBox`, 실제 설정·암호·데이터 기본 위치는 `%ProgramData%\HAS1BlackBox`입니다. 사건 DB는 원본 `blackbox.sqlite3`, 통합 원본 `telemetry.sqlite3`와 **다른 파일**이어야 합니다. 콘솔용 예시의 상대 경로는 config.json 위치 기준입니다. 서비스 재설치는 기존 설정을 보존하므로 새 예시를 복사해도 자동으로 기능이 켜지지 않습니다.

관리자 PowerShell에서 설정을 검사하고 다시 시작합니다.

```powershell
Set-Location "$env:ProgramFiles\HAS1 BlackBox"
.\.venv\Scripts\python.exe collector.py --config "$env:ProgramData\HAS1BlackBox\config.json" --check-config
# 위 검사가 성공한 뒤:
Start-Service HAS1BlackBoxCollector
Get-Content "$env:ProgramData\HAS1BlackBox\data\collector.log" -Tail 40
```

`--check-config`는 구문 검증이며 실제 서버 파일 권한·로그 생성·외부 전송을 확인하지 않습니다. 실행 로그의 `Telemetry counters` 안 `incidents.source_health`와 작업자 상태를 확인합니다. `awaiting_snapshot`은 상태 대기, `healthy`는 최근 확인 성공, `server_stale`·`invalid_snapshot`·`clock_step`은 새 장치 알림을 보류해야 하는 상태입니다. 저장된 과거 `healthy`만 보고 현재 프로세스가 살아 있다고 판단하지 않습니다.

게임 중 참가자의 서버 통신 단절 사건만 처리합니다. 정상적인 게임 종료 후 배터리 분리는 경보가 아니며, 게임 종료·참가 해제·장치 교체에 따른 감시 종료를 실제 재접속 복구와 구분합니다. 생존자·술래·유령 모두 대상이고 `neutral`과 운영자가 음소거한 장치는 제외합니다. 원본 MQTT 로그가 안 들어오는 것만으로 사건을 만들지 않습니다.

### 사건 목록·상태·관련 증거 조회

다음 명령은 기존 사건 DB를 읽기 전용으로 조회합니다. 사건 기능을 켠 뒤 생성된 DB에 사용하며, 조회 명령이 없는 DB를 새로 만들지는 않습니다.

```powershell
Set-Location "$env:ProgramFiles\HAS1 BlackBox"
.\.venv\Scripts\python.exe incidents.py --database "$env:ProgramData\HAS1BlackBox\data\incidents.sqlite3" --stats
.\.venv\Scripts\python.exe incidents.py --database "$env:ProgramData\HAS1BlackBox\data\incidents.sqlite3" --list --limit 20
.\.venv\Scripts\python.exe incidents.py --database "$env:ProgramData\HAS1BlackBox\data\incidents.sqlite3" --telemetry-database "$env:ProgramData\HAS1BlackBox\data\telemetry.sqlite3" --incident-id '목록에서_확인한_사건ID' --limit 200 > "$env:TEMP\has1-incident.json"
```

`--stats`는 사건·발송 상태별 수량과 마지막 기록된 서버 상태를 보여 줍니다. `--list`로 사건 ID를 찾은 뒤 상세 내보내기를 실행합니다. 결과는 원인 확정 보고서가 아니라 사건 상태·전송 이력·관련 로그 후보입니다. 같은 사건 ID는 직접 연결하고, 같은 MAC과 시간 범위는 관련 후보로 구분합니다. 시간 기준이 없는 장치 로그를 업로드 시각으로 사고 시각에 맞추지 않습니다. 기본 최대 200건, `--limit`은 1~1000이며 잘린 결과는 표시합니다.

알림 대기열의 `manual_review`는 외부 전송 후 응답 유실 등으로 실제 전달 여부가 불명확한 상태입니다. Slack/Notion에서 사건 ID를 확인해야 하며 이 항목을 삭제하거나 무조건 재전송하면 중복될 수 있습니다. `permanent`는 인증·설정 등 수정이 필요한 실패입니다. SQLite 사건 기록만으로 외부 API의 정확히 한 번 전달을 보장하지 않습니다.

서버 수집과 정상 배터리 교체 시험을 확인한 뒤 [NOTIFICATIONS.md](NOTIFICATIONS.md)에 따라 Slack·Notion 계정을 설정합니다. 기본 설정 경로가 같으므로 설치된 코드 폴더에서 `configure-notifications.ps1`을 실행하면 됩니다. 이 단계에서 암호는 Windows 숨김 입력창으로 받고, 코드나 채팅에 넣지 않습니다.
