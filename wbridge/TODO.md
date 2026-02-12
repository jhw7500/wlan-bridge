# wlan-bridge 리팩토링 완료 계획

- [x] `refactored/bridge_types.h`에 `bridge_context` 구조체 정의 추가
- [x] `refactored/config.h` & `refactored/config.c` 생성 (설정 파싱)
- [x] `refactored/stats.h` & `refactored/stats.c` 생성 (통계 출력)
- [x] `refactored/bridge.h` & `refactored/bridge.c` 생성 (초기화 및 실행 로직)
- [x] `refactored/main.c` 생성 (진입점)
- [x] `refactored/Makefile` 업데이트
- [x] 빌드 및 유닛 테스트 프레임워크 구축 (주의: 빌드 시 libpcap-dev 필요)
- [x] 모듈화 완료 (main, bridge, config, stats, packet, filter)
- [ ] 실제 환경에서 통합 테스트 및 성능 검증
