# 1호점 Windows 첫 설치

이 폴더는 **처음 설치하는 PC 전용**입니다. MQTT 브로커는 장치 로그를 받는 중계 프로그램이고, 수집기는 받은 내용을 SQLite 파일로 저장합니다. 둘 다 게임 서버와 같은 **172.30.1.43 PC**에서 서비스로 실행합니다. Codex나 Claude를 켜둘 필요는 없습니다.

## 준비물

1. Windows 10/11 64비트 관리자 권한, PC 주소 `172.30.1.43`.
2. [공식 Python Windows 설치 프로그램](https://www.python.org/downloads/windows/)으로 Python 3.11 이상 64비트를 **모든 사용자용, Program Files 아래**에 설치합니다. Microsoft Store/사용자 AppData 설치는 이 서비스에서 지원하지 않습니다.
3. [공식 Mosquitto Windows 설치 프로그램](https://mosquitto.org/download/)으로 Mosquitto를 Program Files 아래에 설치합니다. 기존 브로커가 있다면 이 첫 설치를 실행하지 말고 기존 설정을 먼저 확인합니다. 설치 프로그램이 기본 Mosquitto 서비스를 생성·실행했다면 이 스크립트는 이를 자동으로 중지하거나 대체하지 않습니다. 포트 1883 충돌 여부를 먼저 확인합니다.
4. 릴리스 담당자가 별도로 제공하는 `private-provision.json`. 펌웨어에 들어간 것과 동일한 장치 암호와 별도 수집기 암호를 담습니다. **공개 ZIP/GitHub/채팅에 올리지 않습니다.** 암호를 직접 입력하거나 PowerShell 명령줄에 붙여넣지 않습니다. 파일을 받지 않았다면 설치를 진행하지 않습니다.
5. 장치의 **Wi-Fi STA MAC 목록** 또는 운영자가 확인한 **현재 게임 서버 SQLite 파일 경로**. 서버 DB를 자동으로 찾거나 임의 경로를 가정하지 않습니다. Beetle BLE MAC을 넣으면 안 됩니다.

`private-provision.json`은 `schema_version=1`, `site_id=store-1`, `broker_uri=mqtt://172.30.1.43:1883`, `collector_username=collector`, `device_password`, `collector_password` 필드를 가집니다. 두 암호는 서로 다른 URL-safe ASCII 문자열이며 장치 암호 32~127자, 수집기 암호 32~128자입니다. 파일을 새로 임의 작성하면 펌웨어 암호와 맞지 않을 수 있으므로 제공된 파일을 사용합니다.

## 설치

폴더를 압축 해제하고 **관리자 PowerShell**을 엽니다. 실제 설치된 Python 경로를 확인합니다.

```powershell
Get-ChildItem 'C:\Program Files\Python*\python.exe'
Get-Item 'C:\Program Files\mosquitto\mosquitto.exe'
Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq 1883
```

### 방법 A: MAC 목록 사용

`macs.txt`에 실제 장치 MAC을 한 줄씩 적습니다. 콜론 포함/미포함 모두 지원합니다. 빈 줄과 `#` 주석은 무시합니다. 모든 대상 글러브 TTGO·발전기를 포함해야 합니다. 새 장치를 추가할 때는 별도 계정 추가가 필요하며 이 첫 설치 스크립트를 다시 실행하지 않습니다.

```powershell
Set-Location 'C:\HAS1-BlackBox\windows-blackbox-logger'
.\first-install.ps1 -Provision 'C:\HAS1-Private\private-provision.json' `
  -PythonExe 'C:\Program Files\Python313\python.exe' `
  -MacList '.\macs.txt' -Start
```

위 Python 경로와 압축 해제 폴더는 **예시**이며 실제 설치 경로로 바꿉니다.

### 방법 B: 서버 SQLite에서 등록 MAC 읽기

```powershell
.\first-install.ps1 -Provision 'C:\HAS1-Private\private-provision.json' `
  -PythonExe 'C:\Program Files\Python313\python.exe' `
  -ServerDatabase '운영자가 확인한 실제 SQLite 파일의 전체 경로' -Start
```

`device`와 `training_device`의 `device_type`이 `iotglove` 또는 `generator`인 행에서 `mac`만 읽습니다. DB는 읽기 전용이며 게임 데이터를 변경하지 않습니다. 기본 테이블이 없는 이전 서버라면 실제 존재 여부를 확인하고 `-Tables device`를 지정합니다. 비어 있는 MAC/`mac not found`는 건너뛰고 수량을 출력합니다. 그 외 잘못된 MAC이나 예상하지 못한 스키마는 중단합니다. 여러 테마에 같은 MAC이 등록돼 있으면 하나의 계정으로 합칩니다.

설치 결과를 `C:\ProgramData\HAS1BlackBoxBroker\registered-macs.txt`에서 확인하세요. 동작 중인 장치가 이 목록에 없으면 로그인할 수 없습니다. DB의 MAC이 TTGO의 실제 STA MAC과 맞는지도 확인해야 합니다.

## 설치되는 항목

| 항목 | 위치/동작 |
|---|---|
| `HAS1BlackBoxBroker` 서비스 | 전용 Mosquitto 설정, `172.30.1.43:1883` |
| `HAS1BlackBoxCollector` 서비스 | 기존 WinSW + LocalService + DPAPI 방식 재사용 |
| 브로커 설정·암호 해시 | `C:\ProgramData\HAS1BlackBoxBroker` |
| 수집 DB·진단 DB | `C:\ProgramData\HAS1BlackBox\data` |
| 방화벽 규칙 | 도메인/개인 네트워크, 로컬 서브넷에서 위 IP의 TCP 1883만 허용 |
| 자동 장치 로그 반영·시간 측정 | 활성화 |
| Omada·게임 서버 JSONL·PHP 입력 | 비활성화: 각 장비/서버 전송 설정 이후 별도 활성화 |

장치는 MAC 이름의 계정으로 자기 `batch`에만 쓰고 자기 `ack`만 읽습니다. `collector`는 모든 장치의 batch를 읽고 ack를 보냅니다. 브로커에는 PBKDF2 암호 해시만 저장하고, 수집기 암호는 기존 DPAPI 방식으로 저장합니다. 준비 중 평문 임시 파일은 관리자/SYSTEM만 접근 가능한 새 폴더에 만들고 해시 변환 직후 교체합니다. 제공받은 원본 비공개 JSON은 자동 삭제하지 않으므로 접근을 제한한 별도 보관 위치로 옮기세요.

`-Start`를 생략하면 등록만 하고 실행하지 않습니다. 기존 서비스·설정 폴더·1883 포트가 있으면 덮어쓰지 않고 중단합니다. 중간 실패 시 생성한 파일/서비스는 조사할 수 있게 남습니다. 서비스 존재 여부와 파일을 확인한 뒤 복구해야 하며 폴더를 무조건 지우거나 재실행하지 마세요.

## 현장 확인

```powershell
Get-Service HAS1BlackBoxBroker,HAS1BlackBoxCollector
Get-NetTCPConnection -LocalAddress 172.30.1.43 -LocalPort 1883 -State Listen
Get-NetConnectionProfile
```

게임 LAN이 **공용 네트워크**로 분류되면 설치한 방화벽 규칙으로 장치 접속이 허용되지 않습니다. 신뢰하는 매장 게임 LAN인지 확인한 뒤 Windows에서 개인 네트워크로 설정합니다. 공용/인터넷 전체로 방화벽 범위를 넓히거나 라우터 포트포워딩을 만들지 않습니다. 현재 펌웨어는 평문 MQTT이므로 격리된 매장 LAN에서만 사용합니다.

서비스가 Running인 것만으로 실제 수집 성공을 뜻하지 않습니다. 새 펌웨어를 설치한 시험 장치 한 대로 **게임 대기 상태에서** 로그가 DB에 저장되고 ACK가 반환되는지 확인한 뒤 전체 수집 준비 완료로 판단합니다. 게임 진행 중 업로드는 제한되므로 즉시 로그가 오지 않을 수 있습니다. Omada/서버/PHP까지 준비됐는지는 각각 별도로 확인해야 합니다.

로컬 개발 검증은 실제 Mosquitto 인증·ACL, 읽기 전용 SQLite 인벤토리, 실패 시 임시 평문 정리, PowerShell 구문 검사까지 수행합니다. Windows SCM·실제 방화벽·LocalService 권한·매장 장치 접속은 Windows 현장에서 검증해야 합니다.

Mosquitto 암호 변환 방식은 [공식 mosquitto_passwd 문서](https://mosquitto.org/man/mosquitto_passwd-1.html)의 `-U`를 사용합니다. 암호를 프로세스 인수에 노출하는 `-b`는 설치 스크립트에서 사용하지 않습니다.
