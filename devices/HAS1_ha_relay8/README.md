# ESP32-S3 8채널 릴레이 · ESPHome

ESP32-S3의 릴레이 8개를 Home Assistant에서 **장치 1개와 스위치 8개**로 제어하는 독립 펌웨어입니다. Wi-Fi와 암호화된 ESPHome Native API를 사용하며 MQTT 브로커, 매장 PHP 서버, `HAS2_Wifi` 라이브러리는 필요하지 않습니다. ESPHome 설정의 시작점은 [`esphome/relay8.yaml`](esphome/relay8.yaml)입니다.

## 구성 파일

```text
devices/HAS1_ha_relay8/
├── README.md
└── esphome/
    ├── relay8.yaml           # 보드, Wi-Fi/API, OTA, 릴레이 정의
    ├── requirements.txt      # ESPHome 버전 고정
    ├── secrets.example.yaml  # 비밀정보 형식 예제
    ├── .gitignore
    └── secrets.yaml          # 사용자가 생성, Git 제외
```

공유하는 소스에는 실제 접속정보를 넣지 않습니다. `secrets.yaml`의 Wi-Fi 정보, API 키, OTA 암호는 `!secret`으로 읽습니다. 빌드 산출물에도 이 값이 포함되므로 공개 저장소에 올리지 않습니다.

## 하드웨어와 채널

대상은 **ESP32-S3**, 보드 설정은 `esp32-s3-devkitc-1`입니다. 플래시 4MB를 사용하도록 설정하고 PSRAM은 사용하지 않습니다. 실제 검증 보드는 플래시 16MB와 PSRAM 8MB 모델이며, 이 구성은 해당 보드의 일부 메모리만 사용합니다. 다른 보드에서는 핀 노출과 부가 장치의 핀 공유 여부를 확인합니다.

| Home Assistant 스위치 | GPIO | 활성 레벨 |
|---|---:|---|
| Relay 1 | 18 | LOW |
| Relay 2 | 8 | LOW |
| Relay 3 | 9 | LOW |
| Relay 4 | 10 | LOW |
| Relay 5 | 11 | LOW |
| Relay 6 | 12 | LOW |
| Relay 7 | 13 | LOW |
| Relay 8 | 14 | LOW |

각 채널의 `pin.inverted: true`가 LOW 활성 설정입니다. HIGH 활성 모듈이면 해당 채널을 `false`로 변경합니다. GPIO에는 코일을 직접 연결하지 말고 **3.3V 입력 호환 릴레이 드라이버 모듈**을 연결합니다. 릴레이 전원은 8채널 동시 동작 전류를 감당해야 하며 GPIO에 5V가 들어오면 안 됩니다. 비절연 모듈은 ESP32와 GND를 공통 연결하고, 절연형 모듈은 제조사의 JD-VCC 배선 지침을 따릅니다.

## 동작 정책

- 모든 채널은 `restore_mode: ALWAYS_OFF`로 재부팅 후 OFF로 시작합니다.
- Wi-Fi와 API의 `reboot_timeout: 0s`로 연결 단절에 따른 자동 재부팅을 끕니다. Wi-Fi 또는 HA 연결만 끊기면 마지막 출력을 유지합니다.
- 전원 손실, 수동 재부팅, **OTA 업데이트 후 재부팅 시에는 모든 릴레이가 OFF**가 됩니다. 업데이트 전에 연결된 부하가 꺼져도 되는 상태인지 확인합니다.
- HA 상태는 GPIO 명령 상태입니다. 실제 접점 동작을 읽는 피드백은 없습니다.

부팅 코드 실행 전의 GPIO 부동 상태는 소프트웨어만으로 보장할 수 없습니다. LOW 활성 릴레이 모듈은 적절한 입력 풀업을 사용하고, 최초 확인은 부하 없이 진행합니다.

## 개발 환경과 비밀정보

검증 환경은 **Python 3.14, ESPHome 2026.9.1, ESP-IDF 5.5.5**입니다. ESPHome 버전은 `requirements.txt`에 고정하고, ESP-IDF는 해당 ESPHome 버전의 `recommended` 설정을 사용합니다. 네이티브 ESP-IDF 빌드 도구를 사용하며 최초 빌드에는 툴체인 다운로드를 위한 인터넷 연결이 필요합니다.

다음 명령은 저장소 루트에서 실행합니다.

```sh
python3.14 -m venv build/ha-relay8-esphome/venv
build/ha-relay8-esphome/venv/bin/python -m pip install \
  -r devices/HAS1_ha_relay8/esphome/requirements.txt
```

새 환경에서만 예제 파일을 복사합니다. 기존 `secrets.yaml`이 있다면 덮어쓰지 않습니다.

```sh
cp devices/HAS1_ha_relay8/esphome/secrets.example.yaml \
  devices/HAS1_ha_relay8/esphome/secrets.yaml
openssl rand -base64 32
openssl rand -hex 24
```

`secrets.yaml`에 Wi-Fi SSID와 암호를 입력합니다. 첫 번째 난수 생성 결과는 32바이트 base64 `api_encryption_key`, 두 번째 결과는 `ota_password`에 넣습니다. API 키는 HA의 암호화 연결에 사용하며, OTA 암호는 업데이트 인증에 사용합니다. 각 장치에 고유한 값을 사용합니다.

## 설정 검증과 빌드

```sh
build/ha-relay8-esphome/venv/bin/esphome config \
  devices/HAS1_ha_relay8/esphome/relay8.yaml
build/ha-relay8-esphome/venv/bin/esphome compile \
  devices/HAS1_ha_relay8/esphome/relay8.yaml
```

설정 출력은 비밀정보가 마스킹된 기본 상태를 유지하고 `--show-secrets`를 사용하지 않습니다. 빌드 디렉터리는 `build/ha-relay8-esphome/firmware`입니다. YAML의 상대 `build_path`는 ESPHome의 `.esphome` 데이터 디렉터리를 기준으로 해석됩니다.

| 산출물 | 용도 |
|---|---|
| `build/ha-relay8-esphome/firmware/build/firmware.factory.bin` | 최초 USB 설치용 통합 이미지 |
| `build/ha-relay8-esphome/firmware/build/firmware.ota.bin` | ESPHome이 설치된 장치의 OTA 이미지 |

별도 복사본 없이 원래 빌드 산출물을 사용합니다. OTA 이미지를 flash 주소 0에 단독 기록하지 않습니다.

## 최초 USB 설치와 OTA

처음에는 USB로 연결하고 실제 시리얼 포트를 지정합니다. CLI가 부트로더와 파티션을 포함해 올바른 위치에 기록합니다.

```sh
build/ha-relay8-esphome/venv/bin/esphome upload \
  devices/HAS1_ha_relay8/esphome/relay8.yaml \
  --device /dev/cu.YOUR_ESP32_PORT
```

로거는 S3의 `USB_SERIAL_JTAG`를 사용합니다. 네이티브 USB 포트와 별도 USB-UART 포트가 있는 보드는 포트를 구분합니다.

ESPHome 설치 후 Wi-Fi에 연결되면 `ESP32_IP`를 장치의 실제 IP로 바꿔 OTA 업로드합니다. 설정 변경 시에는 먼저 다시 컴파일합니다.

```sh
build/ha-relay8-esphome/venv/bin/esphome upload \
  devices/HAS1_ha_relay8/esphome/relay8.yaml --device ESP32_IP
build/ha-relay8-esphome/venv/bin/esphome logs \
  devices/HAS1_ha_relay8/esphome/relay8.yaml --device ESP32_IP
```

## Home Assistant 연동

1. 공유기의 DHCP 목록 또는 장치 로그에서 ESP32 자체 IP를 확인합니다. 기본 호스트명은 `has1-relay8`, mDNS 이름은 `has1-relay8.local`입니다.
2. HA 웹 화면 `http://HA_IP:8123`의 설정 → 기기 및 서비스에서 발견된 ESPHome 장치를 추가합니다.
3. 자동 발견이 안 되면 통합 추가 → **ESPHome**을 선택하고 호스트에 `ESP32_IP`, 포트에 **6053**을 입력합니다. HA 서버 IP를 장치 호스트로 입력하지 않습니다.
4. 암호화 키를 요구하면 `esphome/secrets.yaml`의 `api_encryption_key` 값을 입력합니다. HA 로그인 암호나 OTA 암호가 아닙니다.
5. `ESP32-S3 8CH Relay` 장치에 `Relay 1`~`Relay 8` 스위치가 표시되는지 확인합니다.

Docker로 실행하는 HA도 컨테이너에서 ESP32의 LAN IP와 TCP 6053에 접근할 수 있으면 직접 연결합니다. ESPHome 서버나 대시보드를 상시 실행할 필요는 없습니다. mDNS가 Docker 네트워크를 통과하지 못하면 위 수동 IP 등록을 사용합니다. 여러 장치에 설치할 때는 `esphome.name`을 다르게 설정해 호스트명 충돌을 피합니다.

## 검증 범위

실제 ESP32-S3 펌웨어 컴파일, USB 플래시 기록 및 해시 검증, OTA 업데이트 후 새 빌드 확인, 암호화 Native API 재접속과 릴레이 엔터티 8개 응답을 확인했습니다. 물리적 릴레이 접점, 전원 인가 순간의 출력, 통신 단절 중 실제 부하 유지, HA 화면에서의 등록은 별도 현장 검증 항목입니다.

현장에서는 각 채널의 배선 일치, 부팅 시 전 채널 OFF, Wi-Fi/HA 중단 중 출력 유지, 재연결 시 상태 일치, 재부팅 후 OFF를 확인합니다.

공식 문서: [ESPHome ESP32](https://esphome.io/components/esp32/), [Native API](https://esphome.io/components/api/), [GPIO Switch](https://esphome.io/components/switch/gpio/), [ESPHome OTA](https://esphome.io/components/ota/esphome/), [HA ESPHome 통합](https://www.home-assistant.io/integrations/esphome/).
