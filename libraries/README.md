# libraries/

New_HAS1(first_store) 전용으로 이 저장소 안에 내장(vendor)해둔 서드파티/공용 라이브러리.

## HAS2_Wifi

원래 `Fuzzyline-HAS/libraries` 저장소(여러 프로젝트가 공용으로 쓰는 라이브러리 모음)의
`HAS2_Wifi`를 받아 썼는데, New_HAS1(first_store)에서 WiFi 후보 스캔/재연결,
`SetDebugPrint`, `SendAsync`, 더 큰 JSON 버퍼 등 이 프로젝트 전용 개선을 계속 쌓다 보니
공용 저장소의 main과 내용이 갈라졌다. 다른 프로젝트도 같이 쓰는 공용 저장소의 main을
그대로 덮어쓰면 그쪽 사용처에 영향을 줄 수 있어서, 대신 이 폴더에 New_HAS1 전용 사본을
그대로 내장했다.

**즉 여기 있는 HAS2_Wifi는 New_HAS1(first_store) 전용이고, `Fuzzyline-HAS/libraries`의
`first_store` 브랜치와 동기화된 상태다.** 공용 저장소 main이나 다른 브랜치를 신경 쓸
필요 없이, New_HAS1 저장소를 pull하면 이 폴더의 라이브러리가 항상 같이 따라온다.

### CI/배포

`.github/workflows/deploy-firmware.yml`이 이 폴더를 그대로 복사해서 쓴다
(더 이상 외부 저장소를 clone하지 않음).

### 로컬 개발 환경 세팅

Arduino IDE가 스케치북 `libraries/` 폴더에서 라이브러리를 찾으므로, 이 폴더를
그쪽으로 복사(또는 심볼릭 링크)해야 한다:

```powershell
# 예시 (Windows, PowerShell)
Copy-Item -Recurse -Force "libraries\HAS2_Wifi" "$env:USERPROFILE\Documents\Arduino\libraries\HAS2_Wifi"
```

New_HAS1을 새로 clone했거나 HAS2_Wifi를 업데이트했을 때마다 다시 복사해줄 것.
