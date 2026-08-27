# New_HAS1

## ⚠️ HAS2_Wifi 라이브러리 안내 (필독)

**New_HAS1(1호점)의 모든 기기는 반드시 `Fuzzyline-HAS/libraries`의 `HAS2_Wifi` 중
`first_store` 브랜치를 써야 합니다.** 다른 브랜치(예: 다른 매장이 쓰는 `third_store`
등)로 빌드하면 WiFi SSID/서버 IP 같은 매장별 하드코딩 값이 달라서, **컴파일은 되지만
엉뚱한 WiFi/서버에 붙는 식으로 조용히 잘못 동작할 수 있습니다.**

- **로컬 개발**: `libraries/HAS2_Wifi/`는 `first_store` 브랜치를 복사해둔 사본입니다. 로컬 Arduino IDE에서 쓰려면 이 폴더를 Arduino 스케치북의 `libraries/` 폴더로 복사하세요. `first_store`에 새 커밋이 올라와도 **이 사본은 자동으로 안 바뀝니다** — 최신으로 맞추려면 다시 복사해와서 커밋해야 합니다. (자세한 내용은 [`libraries/README.md`](libraries/README.md) 참고)
- **CI/배포**: `.github/workflows/deploy-firmware.yml`은 위 사본을 쓰지 않고, 배포할 때마다 `Fuzzyline-HAS/libraries`의 **`first_store` 브랜치를 매번 새로 `git clone`** 해서 항상 최신 버전으로 빌드합니다.
- 한 컴퓨터에 다른 매장(`third_store` 등) 장치도 같이 빌드한다면, Arduino IDE 전역 스케치북엔 `HAS2_Wifi`라는 이름의 폴더가 하나만 있을 수 있다는 점을 주의하세요 — 지금 무엇이 들어있는지 확인 없이 그냥 복사해 쓰면 다른 매장 장치가 잘못된 브랜치로 빌드될 수 있습니다.
