# 빌드와 릴리즈

## 대상

ESP32 Arduino core는 **3.3.11**을 사용한다. [공유 매핑](../../../scripts/firmware_targets.py)의 FQBN이 로컬 검증과 배포에 함께 적용된다.

| 대상 | 스케치 | 빌드 profile | 고정 Release 태그 |
| --- | --- | --- | --- |
| TTGO T1 본게임 | `devices/iotglove` | `ttgo` | `iotglove` |
| TTGO T1 훈련 | 같은 스케치, `IOTGLOVE_TRAINING=1` | `training` | 없음 — 검증 profile |
| Beetle ESP32-C3 | `devices/iotglove/iotglove_beetle` | `beetle` | `iotglove_beetle` |

두 보드는 `min_spiffs`의 1,966,080-byte OTA 앱 슬롯을 사용한다. Beetle은 ESP32C3 Dev Module, 160MHz, 4MB, USB CDC enabled로 빌드한다. [DFRobot 보드 문서](https://wiki.dfrobot.com/dfr0868/)의 C3/4MB/USB 정보를 바탕으로 core 3.3.11의 `boards.txt` 메뉴를 확인했다. 첫 USB 설치 때 동일 파티션으로 설치해야 한다.

## 소스 파일과 독립 컴파일

TTGO의 `iotglove.ino`는 버전과 `setup()`/`loop()` 진입점을 유지한다. `iotglove.cpp`, `sensor.cpp`, `game_state.cpp`, `wifi.cpp`, `telnet.cpp`는 각각 독립 컴파일되며 헤더로 선언을 공유한다. 기존 장치의 `.ino` 기능 탭과 역할·이름을 맞춘 것이며, `.cpp`를 include하거나 `.ino`로 결합하지 않는다. 서버 인터페이스 헤더는 공식 `WiFi.h`와 이름이 겹치지 않는 `wifi_client.h`다.

Beetle은 `iotglove_beetle.ino`, `iotglove_beetle.h`, `library_and_pin.h`와 해당 폴더의 `.cpp` 파일을 별도로 컴파일한다. TTGO 빌드에 Beetle 소스를 포함하지 않는다. 두 보드 공용 `IoTGloveProtocol` 라이브러리는 기존처럼 명시적으로 제공한다. 호스트 테스트 러너는 같은 `game_state.cpp`를 링크한다. 파일명 정리는 위 스케치 경로·FQBN·Release 태그·버전 증가 규칙에 영향을 주지 않는다.

## 배포 없는 로컬 검증

저장소 루트에서 실행한다. Python 3, C++17 컴파일러, Git, Arduino CLI가 필요하다.

```sh
python3 -m unittest discover -s devices/iotglove/tools -p 'test_*.py'
python3 devices/iotglove/tools/run_tests.py
```

전역 Arduino 라이브러리와 섞이지 않도록 별도 sketchbook을 준비한다. 아래 `IOTGLOVE_ENV`는 새 디렉터리로 지정한다. CLI 데이터 경로는 설치된 core를 공유하고, user 경로만 분리한다.

```sh
export IOTGLOVE_ENV="$(mktemp -d "${TMPDIR:-/tmp}/iotglove-env.XXXXXX")"
arduino-cli config init --dest-file "$IOTGLOVE_ENV/arduino-cli.yaml"
arduino-cli --config-file "$IOTGLOVE_ENV/arduino-cli.yaml" config set directories.user "$IOTGLOVE_ENV/user"
arduino-cli --config-file "$IOTGLOVE_ENV/arduino-cli.yaml" core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli --config-file "$IOTGLOVE_ENV/arduino-cli.yaml" core install esp32:esp32@3.3.11 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli --config-file "$IOTGLOVE_ENV/arduino-cli.yaml" lib install "Adafruit NeoPixel@1.12.0" "ArduinoJson@7.4.3"
python3 devices/iotglove/tools/prepare_libraries.py --libraries-dir "$IOTGLOVE_ENV/custom-libraries"
python3 devices/iotglove/tools/compile.py all \
  --config-file "$IOTGLOVE_ENV/arduino-cli.yaml" \
  --libraries-dir "$IOTGLOVE_ENV/custom-libraries"
```

`all` 대신 `ttgo`, `training`, `beetle` 하나만 지정할 수 있다. `--arduino-cli`, `--jobs`, `--output-dir`도 지원한다. `IOTGLOVE_LIBRARY_DIR` 환경변수는 `--libraries-dir` 기본값이다.

준비 스크립트는 매번 SecureOTA 최신, HAS2_Wifi **first_store 최신**, Arduino-SimpleTimer를 새로 받아 커밋을 기록한다. 글러브의 HAS2_Wifi에만 `has2-wifi-result-api.patch`를 적용하여 응답 성공 여부 및 재부팅 없는 연결 API를 추가한다. 원격 코드와 패치가 맞지 않으면 중단한다. 기존 7개 배포 대상은 패치 없이 기존 방식으로 빌드한다. 기존 라이브러리 디렉터리는 덮어쓰지 않으므로 갱신할 때 새 경로를 준비한다.

검증 빌드는 소스를 임시 스케치로 복사하고 그 안에만 placeholder `secrets.h`를 생성한다. 소스의 실제 `secrets.h`, 버전, 서명, Release를 바꾸지 않는다. 기본 출력은 Git에서 제외되는 `build/iotglove-compile-only/<profile>`이며, `iotglove-dependencies.json`에 실제 사용한 원격 커밋과 패치 해시를 남긴다. **이 placeholder 키 바이너리를 기기에 설치하거나 Release에 올리지 않는다.**

## GitHub Actions

`Compile IoT glove`는 관련 경로의 push/PR 및 수동 실행에서 host test와 세 profile을 빌드한다. 읽기 권한만 사용하며 secrets, 버전 증가, 서명, 업로드, Release 게시를 수행하지 않는다. 빌드 바이너리는 Actions artifact로도 배포하지 않는다.

실제 배포는 기존 `Deploy Firmware`를 수동 실행하여 `iotglove` 또는 `iotglove_beetle`을 선택한다. 배포 전에 `HMAC_SECRET`이 비어 있거나 예제/검증 값이면 실패한다. 최신 SecureOTA의 버전 증가 → 컴파일 → 서명 → 증가된 주 스케치 커밋/푸시 → 고정 태그 Release 갱신 순서를 유지한다. `update_partition`을 선택하면 기존 규칙에 따라 파티션 버전/서명도 배포한다. 수동 배포는 공통 concurrency group으로 직렬화하여 여러 장치의 버전 커밋 및 고정 Release 태그 갱신이 서로 경합하지 않도록 한다. GitHub concurrency는 실행 1개와 대기 1개를 유지하므로 여러 배포를 동시에 대기열에 넣지 않는다.

훈련 profile은 자동 배포 대상이 아니다. 모든 브랜치가 동일 고정 태그를 사용하므로 구현 브랜치 시험에는 `Compile IoT glove`만 사용한다. 실제 Release/OTA 전에 실기기 검증과 서버 연동 확인을 끝낸다.

워크플로 변경은 기본 브랜치에 반영한 뒤 배포한다. GitHub는 기본 브랜치와 다른 워크플로 변경을 포함한 커밋에 새 Release 태그를 만들 때 추가 workflow 권한을 요구할 수 있으며, Actions `GITHUB_TOKEN`으로는 그 권한을 추가할 수 없다. 근거: [GitHub Release 생성 API](https://docs.github.com/en/rest/releases/releases#create-a-release).

## 글러브 버전 보관과 선택

글러브 두 대상은 기존 배포가 성공한 뒤 추가 보관 단계를 실행한다. 예를 들어 `iotglove`의 `NEW_VER=12`는 고정 태그 `iotglove`를 갱신하고 **`iotglove-v12`**도 보관한다. Beetle `NEW_VER=7`은 **`iotglove_beetle-v7`**을 보관한다. 기존 7개 장치에는 이 단계를 적용하지 않는다. 두 대상은 기존처럼 각각 Actions를 실행한다.

보관 자산은 `update.bin`, `update.sig`, `version.txt`, `partition_version.txt`, `ota.txt`, `ota.sig`, `build-provenance.json`이다. 선택한 경우 파티션 파일도 포함하지만 글러브 펌웨어는 파티션 OTA를 실행하지 않는다. 메타데이터 `ota.txt`는 아래 한 줄 형식이며 마지막 LF까지 HMAC-SHA256으로 서명한 32바이트 파일이 `ota.sig`이다.

```text
IGOTA1|iotglove|12|1|min_spiffs|<update.bin의 HMAC-SHA256 소문자 64자리 hex>
```

이미지와 메타데이터는 기존 `HMAC_SECRET`을 사용한다. 메타데이터의 보드·버전·파티션이 요청 및 현재 보드와 맞고, 다운로드한 이미지의 HMAC이 메타데이터에 묶인 값과 맞아야 설치한다. `version.txt`만 바꾸어 다른 버전을 설치할 수 없다.

보관은 빌드 전 소스 해시를 작업 파일 및 커밋과 대조하고 의존성의 커밋·패치 기록을 함께 보관한다. 버전 커밋/푸시가 끝난 정확한 커밋을 태그 대상으로 사용하며, draft에 모든 파일을 올려 다시 읽어 확인한 뒤 공개한다. 기존 파일을 삭제하거나 덮어쓰지 않으며 같은 태그의 재시도는 커밋과 기존 파일 바이트가 모두 일치할 때만 허용한다. 이는 게시 도구의 비덮어쓰기 정책이며 GitHub 관리자 권한 자체를 제한하는 설정은 아니다.

보관 단계가 실패해도 앞선 고정 릴리즈 갱신은 이미 끝났을 수 있다. Actions 실패 로그와 두 릴리즈를 확인하며, 버전 지정은 보관 릴리즈가 완전히 공개된 뒤 사용한다. 예전 고정 태그에서 덮어쓴 바이너리를 자동 복원하거나 채널별 릴리즈를 생성하지 않는다.

사용 예: 서버에서 글러브 `device_state=github@12:7`로 설정하면 Beetle 7 확인/설치 → 실행 버전 확인 → TTGO 12 확인/설치 순서로 진행한다. 같은 버전은 skip하고, 낮은 버전도 명시적으로 지정할 수 있다. `github@12`는 양쪽 12를 의미하며 각각 해당 보관 릴리즈가 필요하다. `github`와 USB `u`는 기존 고정 릴리즈를 사용한다. 없는 버전·잘못된 서명·다른 파티션은 실패하며 최신 릴리즈로 fallback하지 않는다.
