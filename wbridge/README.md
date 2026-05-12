# wbridge

i.MX8MM / i.MX93 + NXP 88W9098 (PCIe/SDIO) 환경의 user-space L2 bridge.

두 가지 엔진을 제공하며 `wifi_init_conf.json` 의 `wbridge.engine` 으로 선택한다.

| 바이너리 | 엔진 | 의존성 | 특징 |
|---|---|---|---|
| `wbridge` | libpcap | `libpcap`, `pthread` | 표준 도구 호환, 디버깅 우수, jitter 안정 |
| `wbridge-tpacket` | AF_PACKET + TPACKET_V3 | `pthread` | RX zero-copy mmap, 발열·idle latency 튜닝 가능 |

> 운영 default 엔진은 NXP moal 드라이버의 kernel-level bridge (`engine=moal`).
> wbridge / wbridge-tpacket 은 정책 적용·측정·폴백 경로에서 사용한다.

## 모듈 구조

```
main.c                   진입점, 시그널 핸들러, SIGUSR1 stats
bridge.h/c               컨텍스트 생성/초기화/스레드 루프
bridge_types.h           타입·enum·구조체 정의 (VLAN, packet_info, config, stats)
config.h/c               env / argv 파싱 + clamp
filter.h/c               MAC / IP / ARP 필터, 멀티캐스트 정책
packet.h/c               802.1Q VLAN 파싱, packet_info 생성
bridge_packet_handler.c  parse → filter → inject 오케스트레이션
stats.h/c                per-interface atomic stats + SIGUSR1 reporter
wbridge-tpacket.c        TPACKET_V3 단독 구현 (단일 파일)
tests/test_filter.c      filter 단위 테스트 (host gcc)
wifi_bridge@.service     systemd unit (mlan0/mlan1 인스턴스)
```

## 빌드

cross-build SDK 설치 후:

```bash
./make-for-imx8  release   # i.MX8MM (sysroot: fsl-imx-xwayland)
./make-for-imx93 release   # i.MX93  (sysroot: fsl-imx-wayland)
```

각 스크립트는 `BOARD_SUFFIX=_imx8` 또는 `_imx93` 을 전달, 산출물은 `release/` 에 보드 suffix 붙여 공존:

```
release/wbridge_imx8         release/wbridge-tpacket_imx8
release/wbridge_imx93        release/wbridge-tpacket_imx93
release/obj_imx8/            release/obj_imx93/
```

host-native (보드에서 직접 빌드):

```bash
make release       # release/wbridge, release/wbridge-tpacket
make tests         # host용 단위 테스트
make run-tests     # 테스트 실행
```

## 환경변수 (요약)

자세한 표는 [`../docs/optimization-modes.md`](../docs/optimization-modes.md) 참조.

| 카테고리 | 변수 |
|---|---|
| Profile 메타 | `WBRIDGE_PROFILE_VERSION`, `WBRIDGE_MODE_REQUESTED`, `WBRIDGE_PROFILE_EFFECTIVE`, `WBRIDGE_THERMAL_STATE`, `WBRIDGE_MODE_FORCE` |
| 공통 런타임 | `WBRIDGE_AFFINITY`, `WBRIDGE_RT`, `WBRIDGE_RT_PRIORITY`, `WBRIDGE_MLOCK`, `WBRIDGE_PROMISC` |
| pcap 엔진 | `WBRIDGE_DISPATCH_BUDGET`, `WBRIDGE_IMMEDIATE`, `WBRIDGE_TIMEOUT_MS`, `WBRIDGE_PCAP_BUFFER`, `WBRIDGE_SNAPLEN` |
| tpacket 엔진 | `WBRIDGE_TPACKET_RETIRE_TOV`, `WBRIDGE_TPACKET_BLOCK_SIZE`, `WBRIDGE_TPACKET_BLOCK_NR`, `WBRIDGE_TPACKET_POLL_TIMEOUT_MS` |
| 필터 | `WBRIDGE_MAC_FILTER`, `WBRIDGE_IP_FILTER`, `WBRIDGE_DEBUG` |

## 단위 테스트

`tests/test_filter.c` — host gcc 로 컴파일되어 host에서 실행. filter 모듈의 multicast/MAC/IP/ARP 필터 동작 검증.

```bash
make run-tests
```

## 관련 문서

- `../docs/optimization-modes.md` — 4가지 모드(latency/normal/eco/thermal) 파라미터, JSON SSoT 구조
- `../docs/driver-options.md` — NXP moal + sysctl + DVFS/ASPM
- `../docs/VLAN-SUPPORT.md` — 802.1Q VLAN 파싱 / IP+MAC 필터
- `../docs/wbridge-smoke-test-checklist.md` — 배포 후 스모크 테스트 항목
