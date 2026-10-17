# libraries/

New_HAS1(first_store) **로컬 개발용**으로 이 저장소 안에 내장(vendor)해둔 사본.
덕트(`HAS1_duct`) CI/배포는 검증된 `LoopFresh` API를 위해, 생명장치
(`HAS1_revival_machine`)는 Wi-Fi 무선 설정 API를 위해 이 폴더를 사용한다.
발전기(`HAS1_generator`)도 HTTP 본문 수신 제한을 위해 이 폴더의 HAS2_Wifi를 사용한다.
다른 장치의 CI/배포는 아래 공용 저장소 정책을 따른다.

## HAS2_Wifi

`Fuzzyline-HAS/libraries` 저장소는 여러 store(프로젝트)가 공용으로 쓰고, 각자 자기
브랜치를 갖는다 — New_HAS1(이 저장소)은 `first_store` 브랜치, New_HAS1 밖에 있는 다른
장치/저장소들은 `third_store` 등 자기 브랜치를 쓴다. 공용 저장소의 main을 직접
건드리면 다른 store에 영향을 줄 수 있어서, New_HAS1 전용 브랜치(`first_store`)로
따로 관리한다.

**덕트·생명장치·발전기를 제외한 CI(`deploy-firmware.yml`)는 배포할 때마다 `first_store` 브랜치를 GitHub에서 새로
클론해서 쓴다 — 항상 최신 버전.** 반면 이 폴더(`libraries/HAS2_Wifi`)는 로컬 Arduino
IDE로 개발할 때 편하게 쓰라고 넣어둔 **스냅샷 사본**이라, `first_store`에 새 커밋이
올라와도 자동으로 안 따라온다. 최신으로 맞추려면 `Fuzzyline-HAS/libraries`의
`first_store`에서 다시 복사해와서 커밋해야 한다.

### 로컬 개발 환경 세팅

Arduino IDE가 스케치북 `libraries/` 폴더에서 라이브러리를 찾으므로, 이 폴더를
그쪽으로 복사해야 한다:

```powershell
# 예시 (Windows, PowerShell)
Copy-Item -Recurse -Force "libraries\HAS2_Wifi" "$env:USERPROFILE\Documents\Arduino\libraries\HAS2_Wifi"
```

New_HAS1을 새로 clone했을 때 한 번 해주면 되고, 이 폴더가 나중에 업데이트되면 다시
복사해줄 것.

**주의**: 스케치북 `libraries/` 폴더엔 `HAS2_Wifi`라는 이름의 폴더가 하나만 있을 수
있다. 한 컴퓨터에서 다른 매장(`third_store` 등) 장치도 같이 빌드한다면, 지금
`libraries/HAS2_Wifi`에 어느 브랜치 사본이 들어있는지 확인하지 않고 그대로 쓰면 안
된다 — New_HAS1 작업할 땐 반드시 이 저장소의 `first_store` 사본으로 되어 있는지
확인할 것.

## 발전기 의존성 고정 및 로컬 빌드

발전기는 `HAS2_Wifi::EnableBoundedResponseBody()`로 본문 수신의 전체 시간과 크기 제한을
활성화한다. 기본값은 꺼져 있으므로 다른 장치의 통신 동작은 유지된다.
발전기 릴리즈는 이 저장소의 `HAS2_Wifi`를 복사하고, 아래 DFPlayer 사본을
`--library`로 명시해 레지스트리 1.0.6보다 우선 선택한다.

`libraries/DFRobotDFPlayerMini`는 upstream 1.0.6을 기반으로 ACK 대기의 전체 시간
제한과 수신 처리량 제한을 추가한 사본이다. 원본 출처와 라이선스는 해당 폴더에 보존한다.
로컬 발전기 빌드에서도 두 라이브러리를 명시해야 한다:

```sh
arduino-cli compile --fqbn '<TTGO FQBN>' \
  --library libraries/HAS2_Wifi \
  --library libraries/DFRobotDFPlayerMini \
  --library libraries/HAS1BleBeacon devices/HAS1_generator
```

Arduino IDE에서는 기존 스케치북의 두 라이브러리를 이 사본으로 교체한 뒤 빌드한다.
