# Blackbox Phase 1

글러브 TTGO·발전기 로그를 별도 라이브러리에 저장하고, 게임이 아닌 로그만 MQTT로 회수합니다.

- [구현·검증 기록](VALIDATION.md)
- [Windows 서비스 추가 검증](WINDOWS_SERVICE_VALIDATION.md)
- [1호점 Omada·서버 상관 로그 구현 및 현장 적용](CORRELATION_VALIDATION.md)
- [실행 계획](../../.omc/plans/2026-10-24-blackbox-phase1-plan.md)
- [장치 라이브러리·저장 형식](../../libraries/HAS1BlackBox/README.md)
- [글러브 설정과 이벤트 해석](../../devices/iotglove/docs/BLACKBOX.md)
- [발전기 설정과 이벤트 해석](../../devices/HAS1_generator/BLACKBOX.md)
- [Windows 수집기 설치·실행·조회](../../tools/windows-blackbox-logger/README.md)

기본값은 flash-only입니다. 현장 MQTT를 사용하려면 장치의 broker URI/계정과
Windows Mosquitto·수집기 설정이 필요합니다. 기존 게임 통신·OTA 구조는 유지합니다.
파티션이 가득 차면 미수집 기록도 오래된 순서로 덮어쓰며, 몇 주 기록은 Windows에 회수하여 보관합니다.
검증용 HMAC 키로 만든 compile-only 결과물은 배포하지 않습니다.
