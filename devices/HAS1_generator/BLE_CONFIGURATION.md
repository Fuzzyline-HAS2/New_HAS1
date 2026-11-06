# 발전기 BLE 설정

발전기 펌웨어의 기본값은 `HAS1_GENERATOR_BLE_ENABLED=0`이다. 이 설정은
BLE 광고만 중단하는 것이 아니라 `HAS1BleBeacon` 라이브러리를 포함하지 않아
Bluetooth 컨트롤러 초기화와 HCI 명령 전송을 실행하지 않는다. 다른 장치의
공용 BLE 라이브러리와 기본 동작은 변경하지 않는다.

블랙박스는 기존 `HAS1_GENERATOR_BLACKBOX_ENABLED=1`을 유지한다. 로컬 기록,
설정된 MQTT 업로드, 텔넷, Wi-Fi, 배선, RFID, 스타터 및 OTA는 유지한다.
BLE 비콘을 이용하는 외부 감지 기능은 발전기를 검색할 수 없게 된다.

부팅 UART/텔넷 이력에 `[BLE] disabled; controller startup excluded`가 남고,
텔넷 연결 시 진단 행에 `ble=disabled`가 표시된다. 블랙박스 부팅 기록 다음의
`build.ble.blackbox` 이벤트는 첫 번째 값에 BLE 설정(0), 두 번째 값에 블랙박스
설정(1)을 기록한다. 펌웨어 버전은 정식 릴리즈 절차에서 증가시킨다.

정식 배포 워크플로 입력은 `device=HAS1_generator`, `blackbox_mode=mqtt`,
`update_partition=false`, `republish` 공란으로 지정한다. `blackbox_mode`의
워크플로 기본값인 `flash`를 그대로 사용하면 MQTT 업로드가 빠지므로 반드시
`mqtt`를 명시한다. 기존 버전 재발행 없이 릴리즈 절차로 버전을 증가시킨다.

GitHub OTA에서 BLE 정지/메모리 해제 단계는 비활성 설정에서 즉시 반환한다.
이미 시작하지 않은 컨트롤러를 정지하거나 메모리를 다시 해제하지 않는다.
기존 OTA 제한 시간과 블랙박스 업로드 일시 정지/재개 동작은 유지한다.
BLE 사용 빌드가 필요하면 컴파일 정의 `HAS1_GENERATOR_BLE_ENABLED=1`을
명시하여 기존 컨트롤러 호출 경로를 사용한다.

이번 변경은 SG의 Bluetooth 초기화 크래시 경로를 제거한다. 최초 메모리
손상 원인이 확정된 것은 아니며, 다른 리셋 원인까지 해결됐다는 의미는
아니다. 사용하지 않는 Bluetooth 메모리가 회수되어 힙 배치도 달라지므로,
리셋이 사라지더라도 BLE 자체가 유일한 원인이었다고 단정할 수 없다.
후속 분석에는 블랙박스의 재부팅 원인, 크래시 PC/태스크, 메모리 상태와
HTTP/Wi-Fi 이벤트를 계속 비교한다.

검증은 `tests/run_ble_config_tests.py`와 `tests/run_boot_network_tests.py`로
비활성/활성 경로 및 블랙박스 시작을 확인하고, `tests/run_network_tests.py`로
두 모드의 OTA 호출 순서와 네트워크 회귀를 확인한다. 호스트 테스트는 실제
ESP32 부팅, RF 환경 및 OTA 하드웨어 검증을 대체하지 않는다.
