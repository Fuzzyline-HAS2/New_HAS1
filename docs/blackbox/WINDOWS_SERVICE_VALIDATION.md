# Windows 수집기 서비스 — 검증 기록

2026-10-24 / `codex/blackbox-mqtt`.

**구현·macOS 자동 검증·독립 검토 완료. 실제 Windows SCM 실행은 미실행.** 기존 블랙박스 펌웨어 검증은 [VALIDATION.md](VALIDATION.md)에 별도 기록한다.

## 범위

Windows 부팅 시 수집기 자동 실행, 비정상 종료 후 재시작, 설치/시작/중지/재시작/상태/삭제 명령.
Slack·Notion 전송, 설정 GUI, Mosquitto 서비스 설치와 방화벽 설정은 이번 변경에 포함하지 않는다.

- 기존 Python/paho/SQLite 수집기를 WinSW 2.12.0 고정 버전으로 실행한다.
- LocalService 실행 계정, Program Files 프로그램/ProgramData 운영 데이터 분리.
- 서비스용 MQTT 암호는 Windows DPAPI LocalMachine과 파일 ACL을 함께 사용한다.
- 수동 실행과 서비스 실행의 동일 DB 중복 소유를 OS 파일 잠금으로 막는다.
- 정상 중지 시 이미 수락한 배치를 처리하며, SQLite COMMIT 이후 ACK 계약을 유지한다.
- WinSW 기본 Ctrl+C와 자식 프로세스 우선 종료, 프로세스별 30초 timeout을 사용한다. 관리 명령의 SCM 상태 확인은 최대 90초 대기한다. 이는 전체 종료가 30초 이내라는 보장이 아니다. 2.12의 stoparguments 경로는 timeout이 적용되지 않아 사용하지 않는다.

## 실행한 검증

| 항목 | 결과 |
|---|---|
| 수집기 전체 | 42개 실행: 40개 통과, Windows 전용 2개 skip (8.376초) |
| 실제 MQTT | Mosquitto 2.0.22 인증/ACL/중복·재전송/브로커 재시작 통과 |
| 신규 서비스 시험 | 11개 중 호스트에서 9개 통과; Windows DPAPI·전역 mutex 2개 미실행 |
| 종료·동시성 | 수락 중 종료 경쟁, 수락한 배치 drain, COMMIT 이후 ACK, 프로세스 강제 종료 후 DB 잠금 해제 통과 |
| PowerShell 구문 | service.ps1, tests/windows_service_smoke.ps1 모두 PowerShell 7.4.21 파서 통과. Windows PowerShell 5.1 실제 실행과 구분 |
| 독립 검토 | 종료 대기·권한·암호·설치 실패 rollback·삭제 시 보존 검토 및 수정 후 승인 |
| 변경 격리 | 이전 펌웨어·공용 라이브러리 등 44개 변경 파일의 SHA256 일치; 이번 작업에서 변경 없음 |
| Windows SCM smoke CI | 설치·LocalService DB 생성·DPAPI·중복 거부·crash recovery·정상 중지·삭제/재등록 시험 추가. 실행 결과는 아직 없음 |

검토 중 WinSW 2.12의 stop 명령이 종료 완료를 기다리지 않는 점을 확인해 SCM 상태 대기를 추가했다.
또한 stoparguments 경로의 timeout 미적용을 피하고, venv의 부모/자식 프로세스를 고려해 자식부터 종료하도록 구성했다.
WinSW timeout은 **프로세스별** 값이므로 전체 서비스가 반드시 30초 안에 종료된다고 표시하지 않는다.

### 재실행

```sh
python -m unittest discover -s tools/windows-blackbox-logger/tests -v
```

실제 MQTT 시험에는 paho-mqtt와 Mosquitto가 필요하다. 없으면 skip되므로 별도 확인한다.
Windows SCM smoke는 `.github/workflows/blackbox-logger-tests.yml`의 `windows-service` job에서
일회용 관리자 Windows runner로 실행한다. 정상 단위 시험은 서비스를 설치하지 않는다.
`tests/windows_service_smoke.ps1`은 시험 설치와 정리를 수행하므로 운영 PC에서는 실행하지 않는다.

## 실제 Windows에서 확인할 항목

현재 개발 호스트는 macOS이다. 호스트 단위 시험만으로 Windows SCM 실행 성공을 주장하지 않는다.

1. 관리자 설치, 제한 계정 접근 권한, 암호 보호 및 복호화.
2. 로그인 전 자동 시작과 부팅 후 수집 재개.
3. 정상 중지, 재시작, 비정상 종료 후 서비스 복구.
4. 동일 DB 이중 실행 거부, 브로커 재연결과 ACK/중복 제거.
5. 삭제 후 DB·로그·설정 보존.

## 공식 근거

- [WinSW 2.12 XML 설정](https://github.com/winsw/winsw/blob/v2.12.0/doc/xmlConfigFile.md)
- [WinSW 종료 처리](https://github.com/winsw/winsw/blob/v2.12.0/src/WinSW/WrapperService.cs)
- [Microsoft DPAPI CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)

LocalMachine DPAPI는 같은 컴퓨터에서 blob을 읽을 수 있는 다른 계정도 복호화할 수 있으므로 파일 ACL이 함께 필요하다.
