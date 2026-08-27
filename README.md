# New_HAS1

## ⚠️ HAS2_Wifi 라이브러리 안내 (필독)

- **로컬 개발**: `libraries/HAS2_Wifi/`는 `Fuzzyline-HAS/libraries`의 **`first_store` 브랜치를 복사해둔 사본**입니다. 로컬 Arduino IDE에서 쓰려면 이 폴더를 Arduino 스케치북의 `libraries/` 폴더로 복사(또는 심볼릭 링크)하세요. `first_store`에 새 커밋이 올라와도 **이 사본은 자동으로 안 바뀝니다** — 최신으로 맞추려면 다시 복사해와서 커밋해야 합니다. (자세한 내용/복사 명령어는 [`libraries/README.md`](libraries/README.md) 참고)
- **CI/배포**: `.github/workflows/deploy-firmware.yml`은 위 사본을 쓰지 않고, 배포할 때마다 `Fuzzyline-HAS/libraries`의 **`first_store` 브랜치를 매번 새로 `git clone`** 해서 항상 최신 버전으로 빌드합니다.
- `Fuzzyline-HAS/libraries`는 여러 store(프로젝트)가 공용으로 쓰는 저장소라, New_HAS1(first_store) 전용 변경사항은 반드시 `first_store` 브랜치에만 반영하세요. 다른 store들은 각자 자기 브랜치(예: `third_store`)를 씁니다.
