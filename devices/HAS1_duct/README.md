# HAS2 Duct

## Ver 0.1

## TTGO T1 USB 자동 업로드

`scripts/upload.py`는 `build/esp32.esp32.ttgo-t1`의 검증된 전체 빌드를
자동 인식한 TTGO T1 포트에 460800 baud로 업로드하고 플래시를 검증합니다.
빌드 자체는 수행하지 않습니다.

```bash
# 실제 플래시 없이 빌드/설정 검사
python3 scripts/upload.py --dry-run

# 연결된 보드가 하나일 때 자동 인식 후 업로드
python3 scripts/upload.py

# 포트가 여러 개일 때 대상 지정
python3 scripts/upload.py --port /dev/cu.SLAB_USBtoUART

# 사용자 확인을 생략하는 자동화 모드(대상 보드를 확실히 식별한 경우만)
python3 scripts/upload.py --port /dev/cu.SLAB_USBtoUART --yes
```

다음 조건에서는 장치 보호를 위해 업로드를 중단합니다.

- 빌드 파일이 없거나 소스보다 오래된 경우
- 빌드 이후 실제 사용한 외부 라이브러리 소스·헤더가 변경된 경우
- `HAS2_Wifi::Situation` 3인자 구현이 빌드에 포함되지 않은 경우
- `secrets.h` 또는 `scripts/secrets.py`가 예제 키이거나 두 키가 다른 경우
- 포트가 여러 개이거나 다른 프로그램이 포트를 점유한 경우
