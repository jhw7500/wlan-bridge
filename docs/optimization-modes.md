# wlan-bridge 최적화 모드 참조

i.MX8MM + NXP 88W9098 (PCIe) WiFi 브릿지 환경

## 스크립트 사용법

```bash
sudo ./setup-irq-affinity.sh --mode <MODE> <eth_if> <wlan_if>
```

---

## 모드별 비교표

### setup-irq-affinity.sh (ethtool)

| 항목 | latency | normal | thermal |
|---|---|---|---|
| **rx-usecs** | 0 | 50 | 150 |
| **tx-usecs** | 0 | 50 | 150 |
| **rx-frames** | 1 | 4 | 10 |
| **GRO** | off | on | on |
| **GSO** | off | off | off |
| **TSO** | off | off | off |
| **인터럽트 빈도** (100Mbps) | ~8,000/s | ~2,000/s | ~200/s |
| **추가 레이턴시** | 0us | ~50us | ~150us |
| **발열** | 높음 | 중간 | 낮음 |

### wbridge 환경변수

#### wbridge-pcap용 (libpcap 기반)

| 환경변수 | latency | normal (기본값) | thermal | 설명 |
|---|---|---|---|---|
| `WBRIDGE_DISPATCH_BUDGET` | 64 | 64 | **128** | pcap_dispatch 1회당 최대 패킷 수 |
| `WBRIDGE_IMMEDIATE` | 1 | 1 | **0** | pcap immediate mode (패킷 도착 즉시 전달) |
| `WBRIDGE_TIMEOUT_MS` | 1 | 1 | **10** | pcap 폴링 타임아웃 (ms) |
| `WBRIDGE_RT_PRIORITY` | **80** | 50 | **30** | SCHED_FIFO 우선순위 (1~99) |
| `WBRIDGE_PCAP_BUFFER` | 4194304 | 4194304 | **8388608** | pcap RX 버퍼 크기 (bytes) |

#### wbridge-tpacket용 (TPACKET_V3 기반)

| 환경변수 | latency | normal (기본값) | thermal | 설명 |
|---|---|---|---|---|
| `WBRIDGE_TPACKET_RETIRE_TOV` | 1 | 1 | **10** | Block retire timeout (ms) |
| `WBRIDGE_RT_PRIORITY` | **80** | 50 | **30** | SCHED_FIFO 우선순위 (1~99) |

> **참고:** `wbridge-tpacket`은 TPACKET_V3 zero-copy mmap을 사용하므로 버퍼 크기와 dispatch budget이 필요 없습니다. Block 배치 처리로 자동 최적화됩니다.

### 왜 달라지는가

| 환경변수 | latency에서 | thermal에서 |
|---|---|---|
| **DISPATCH_BUDGET=64** | 적은 양을 자주 처리 → 지연 최소 | - |
| **DISPATCH_BUDGET=128** | - | 한번에 많이 처리 → wakeup 횟수 감소 |
| **IMMEDIATE=1** | 패킷 도착 즉시 pcap에서 반환 | - |
| **IMMEDIATE=0** | - | timeout까지 대기 후 배치 반환 → CPU idle 확보 |
| **TIMEOUT_MS=1** | 1ms마다 깨어남 → 빠른 반응 | - |
| **TIMEOUT_MS=10** | - | 10ms 간격 → CPU가 C-state 진입 가능 |
| **RT_PRIORITY=80** | 높은 우선순위 → 선점 스케줄링 유리 | - |
| **RT_PRIORITY=30** | - | 낮은 우선순위 → 스케줄러 부하 감소 |
| **PCAP_BUFFER=8MB** | - | 병합으로 burst 도착 → 큰 버퍼로 드롭 방지 |

### 공통 설정 (모드 무관)

| 항목 | 값 | 설명 |
|---|---|---|
| IRQ Affinity ETH | CPU 2 | `echo 4 > /proc/irq/<N>/smp_affinity` |
| IRQ Affinity WLAN | CPU 3 | `echo 8 > /proc/irq/<N>/smp_affinity` |
| RPS ETH | CPU 2 | `echo 4 > /sys/class/net/eth0/queues/rx-0/rps_cpus` |
| RPS WLAN | CPU 3 | `echo 8 > /sys/class/net/mlan0/queues/rx-0/rps_cpus` |
| Ring Buffer | rx:4096 tx:4096 | `ethtool -G <if> rx 4096 tx 4096` |
| RX/TX Checksum | on | `ethtool -K <if> rx on tx on` |
| WBRIDGE_AFFINITY | 1 | Thread 0→CPU 0, Thread 1→CPU 1 |
| WBRIDGE_RT | 1 | SCHED_FIFO 활성 |
| WBRIDGE_MLOCK | 1 | 메모리 잠금 (page fault 방지) |
| WBRIDGE_SNAPLEN | 1600 | 패킷 캡처 길이 |
| WBRIDGE_PROMISC | 1 | 무차별 모드 |

---

## 모드별 실행 예시

### latency 모드 (레이턴시 최소화)

```bash
# 1. 네트워크 설정
sudo ./setup-irq-affinity.sh --mode latency eth0 mlan0

# 2. wbridge 실행
WBRIDGE_DISPATCH_BUDGET=64 \
WBRIDGE_IMMEDIATE=1 \
WBRIDGE_TIMEOUT_MS=1 \
WBRIDGE_RT_PRIORITY=80 \
  wbridge eth0 mlan0
```

**용도:** 실시간 제어, 지연에 민감한 트래픽
**특성:**
- 패킷 도착 즉시 인터럽트 (rx-usecs=0)
- pcap immediate mode ON (패킷 즉시 전달)
- GRO OFF (패킷 병합 없이 개별 처리)
- RT 우선순위 80 (공격적 선점)
- 최소 지연, 최대 발열

### normal 모드 (균형)

```bash
# 1. 네트워크 설정
sudo ./setup-irq-affinity.sh eth0 mlan0           # --mode normal 생략 가능

# 2. wbridge 실행 (기본값 사용)
wbridge eth0 mlan0
```

**용도:** 일반적인 브릿지 운용
**특성:**
- 50us 병합 또는 4패킷마다 인터럽트
- GRO ON (수신 패킷 병합)
- RT 우선순위 50 (기본)
- 적절한 레이턴시와 발열의 균형

### thermal 모드 (발열 최소화)

```bash
# 1. 네트워크 설정
sudo ./setup-irq-affinity.sh --mode thermal eth0 mlan0

# 2. wbridge-pcap 실행
WBRIDGE_DISPATCH_BUDGET=128 \
WBRIDGE_IMMEDIATE=0 \
WBRIDGE_TIMEOUT_MS=10 \
WBRIDGE_RT_PRIORITY=30 \
WBRIDGE_PCAP_BUFFER=8388608 \
  wbridge eth0 mlan0

# 2-1. wbridge-tpacket 실행 (tpacket 기반)
WBRIDGE_TPACKET_RETIRE_TOV=10 \
WBRIDGE_RT_PRIORITY=30 \
  dumb-tpacket eth0 mlan0
```

**용도:** 고온 환경, Commercial grade SoC, 밀폐 장치
**특성:**
- 150us 병합 또는 10패킷마다 인터럽트
- **pcap**: immediate mode OFF (배치 전달)
- **tpacket**: retire_tov=10ms (Block 수집 기간 늘림)
- dispatch_budget=128 (pcap: 한번에 더 많은 패킷 처리)
- timeout 10ms (pcap: 폴링 간격 확대로 CPU idle 확보)
- RT 우선순위 30 (스케줄러 부하 감소)
- pcap 버퍼 8MB (burst 패킷 수용)
- **tpacket은 zero-copy mmap으로 더 낮은 오버헤드**
- 최소 발열, 추가 레이턴시 ~150us

---

## 모드 전환

런타임에 ethtool 설정은 즉시 변경 가능합니다. wbridge는 재시작이 필요합니다.

현재 구현은 온도 센서를 직접 읽어 동적으로 모드를 바꾸는 방식이 아닙니다.
`WBRIDGE_THERMAL_STATE` 입력값을 서비스 시작 시점에 1회 반영해 effective 모드를 결정합니다.

```bash
# thermal → latency 전환
sudo ./setup-irq-affinity.sh --mode latency eth0 mlan0

# wbridge 재시작
systemctl stop wifi_bridge@mlan0
WBRIDGE_IMMEDIATE=1 WBRIDGE_TIMEOUT_MS=1 WBRIDGE_RT_PRIORITY=80 \
  systemctl start wifi_bridge@mlan0
```

## 정책 메타데이터 키

`setup-irq-affinity.sh`가 `/run/wbridge.env`에 아래 키를 기록합니다.

| 키 | 의미 |
|---|---|
| `WBRIDGE_PROFILE_VERSION` | 프로파일 스키마 버전 |
| `WBRIDGE_MODE_REQUESTED` | 요청 모드 (`latency`/`normal`/`thermal`) |
| `WBRIDGE_PROFILE_EFFECTIVE` | 실제 적용 모드 (thermal 상태에 따라 clamp 가능) |
| `WBRIDGE_THERMAL_STATE` | 열 상태 힌트 (`ok`/`warm`/`hot`) |
| `WBRIDGE_MODE_FORCE` | 강등 우회 플래그 (`0`: clamp 적용, `1`: 요청 모드 강제) |

`wifi_bridge.sh`는 시작 시 아래 규칙으로 effective 모드를 계산합니다.

- `WBRIDGE_MODE_FORCE=1` 이면 요청 모드를 그대로 적용
- `WBRIDGE_MODE_FORCE=0`일 때 `hot` -> `thermal`
- `WBRIDGE_MODE_FORCE=0`일 때 `warm` + `latency` 요청 -> `normal`
- 그 외 -> 요청 모드 그대로 적용

effective 값은 `/run/wbridge.effective.json`에도 기록됩니다.

## 운영 사용법 (systemd)

`/etc/default/wbridge` 또는 유닛 오버라이드에 아래 키를 설정합니다.

```bash
WBRIDGE_OPTIMIZE=1
WBRIDGE_MODE=normal
WBRIDGE_ENGINE=pcap
WBRIDGE_THERMAL_STATE=ok
WBRIDGE_MODE_FORCE=0
WBRIDGE_PROFILE_VERSION=1
```

thermal 상태에서 자동으로 공격적 UDP 튜닝(`optimize-for-udp.sh`)은 스킵됩니다.
단, `WBRIDGE_MODE_FORCE=1`이면 thermal에서도 강제 실행됩니다.

엔진 선택:

- `WBRIDGE_ENGINE=pcap` -> `/usr/local/bin/wifi-wbridge --ip-filter --no-debug` 실행
- `WBRIDGE_ENGINE=tpacket` -> `/usr/local/bin/wifi-wbridge-tpacket` 실행

적용 상태 파일:

- `/run/wbridge.effective.json`: 요청/적용 모드와 주요 파라미터 스냅샷
- `/run/wbridge.apply.json`: UDP/IRQ 최적화 적용 결과와 선택 엔진

## 검증 명령

```bash
# effective 프로파일 확인
cat /run/wbridge.env
cat /run/wbridge.effective.json

# 서비스 로그에서 profile/config 라인 확인
journalctl -u wifi_bridge@mlan0 -n 100 --no-pager | grep -E "Profile:|effective="

# tpacket 실행 시 profile 로그 확인
journalctl -t dumb-tpacket -n 50 --no-pager
```

종합 스모크 테스트:

```bash
sudo /usr/local/scripts/wbridge_smoke_test.sh mlan0
```

## Thermal state updater (옵션)

주기적으로 온도 센서를 읽어 `/run/wbridge.thermal.env`의 `WBRIDGE_THERMAL_STATE`를 갱신하고,
상태가 바뀌면 `wifi_bridge@mlan0/1` 재시작으로 모드를 재적용합니다.

```bash
# 활성화
systemctl enable --now wbridge-thermal-state.timer

# 비활성화 (요청하신 것처럼 꺼도 기본 브리지 동작에는 영향 없음)
systemctl disable --now wbridge-thermal-state.timer wbridge-thermal-state.service

# 상태 확인
systemctl status wbridge-thermal-state.timer wbridge-thermal-state.service
cat /run/wbridge.thermal.env
```

기본 정책:

- 입력: CPU thermal zone + mlan0/mlan1 센서
- 출력: `ok`/`warm`/`hot` (hysteresis 적용)
- 상태 변경 시: bridge 재시작(기본)
- `WBRIDGE_MODE_FORCE=1`: 상태는 기록하지만 clamp 기반 재시작은 건너뜀

주요 설정(`/etc/default/wbridge`):

- `WBRIDGE_THERMAL_AUTO_RESTART=1`
- `WBRIDGE_THERMAL_TIMER_ENABLE=1`
- `WBRIDGE_THERMAL_RESTART_COOLDOWN_SEC=60`
- 필요 시 임계값 조정: `WBRIDGE_THERMAL_WARM_CPU_ENTER`, `WBRIDGE_THERMAL_HOT_CPU_ENTER`, `WBRIDGE_THERMAL_WARM_CPU_EXIT`, `WBRIDGE_THERMAL_HOT_CPU_EXIT`

---

## 모니터링 명령어

```bash
# 인터럽트 빈도 확인 (2초 간격)
watch -n2 'cat /proc/interrupts | grep -E "eth|mlan|pcie"'

# SoC 온도 확인
cat /sys/class/thermal/thermal_zone0/temp

# CPU 주파수 확인
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq

# ethtool 현재 coalescing 확인
ethtool -c eth0
ethtool -c mlan0

# wbridge 통계
kill -USR1 $(pidof wbridge)    # syslog로 통계 출력
```
