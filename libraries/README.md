# libraries/

New_HAS1(first_store) **로컬 개발용**으로 이 저장소 안에 내장(vendor)해둔 사본.
CI/배포는 이 폴더를 쓰지 않는다 (아래 참고).

## HAS2_Wifi

`Fuzzyline-HAS/libraries` 저장소는 여러 store(프로젝트)가 공용으로 쓰고, 각자 자기
브랜치를 갖는다 — New_HAS1(이 저장소)은 `first_store` 브랜치, New_HAS1 밖에 있는 다른
장치/저장소들은 `third_store` 등 자기 브랜치를 쓴다. 공용 저장소의 main을 직접
건드리면 다른 store에 영향을 줄 수 있어서, New_HAS1 전용 브랜치(`first_store`)로
따로 관리한다.

**CI(`deploy-firmware.yml`)는 배포할 때마다 `first_store` 브랜치를 GitHub에서 새로
클론해서 쓴다 — 항상 최신 버전.** 반면 이 폴더(`libraries/HAS2_Wifi`)는 로컬 Arduino
IDE로 개발할 때 편하게 쓰라고 넣어둔 **스냅샷 사본**이라, `first_store`에 새 커밋이
올라와도 자동으로 안 따라온다. 최신으로 맞추려면 `Fuzzyline-HAS/libraries`의
`first_store`에서 다시 복사해와서 커밋해야 한다.

### 로컬 개발 환경 세팅

Arduino IDE가 스케치북 `libraries/` 폴더에서 라이브러리를 찾으므로, 이 폴더를
그쪽으로 복사(또는 심볼릭 링크)해야 한다:

```powershell
# 예시 (Windows, PowerShell)
Copy-Item -Recurse -Force "libraries\HAS2_Wifi" "$env:USERPROFILE\Documents\Arduino\libraries\HAS2_Wifi"
```

New_HAS1을 새로 clone했을 때 한 번 해주면 되고, 이 폴더가 나중에 업데이트되면 다시
복사해줄 것.
