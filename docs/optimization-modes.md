# wlan-bridge 최적화 모드 참조

i.MX8MM + NXP 88W9098 (PCIe) WiFi 브릿지 환경

## 스크립트 사용법

```bash
sudo ./setup-irq-affinity.sh --mode <MODE> <eth_if> <wlan_if>
```

---

## 설정 구조 및 조합

### JSON 설정 구조 (`wifi_init_conf.json → wbridge`)

```
wbridge
├── enabled              # bridge 전체 ON/OFF (마스터 스위치)
├── bridge_iface         # bridge 인터페이스 (mlan0/mlan1)
├── engine               # pcap | tpacket
├── optimize
│   ├── enabled          # 커널 레벨 튜닝 ON/OFF
│   ├── mode             # latency | normal | eco | thermal
│   └── irq_affinity     # auto | pinned | none
├── link_guard
│   └── enabled          # 링크 감시 ON/OFF
└── thermal
    └── mode_force       # thermal 클램핑 무시 여부
```

### 설정 조합별 동작

#### 1단계: `wbridge.enabled`

| 값 | 동작 |
|---|---|
| `true` | bridge 서비스 활성, 아래 설정 적용 |
| `false` | bridge 서비스 전체 stop+disable, 이하 모든 설정 무효 |

#### 2단계: `optimize.enabled` — 커널 튜닝 제어

| `optimize.enabled` | 적용되는 것 | 적용 안 되는 것 |
|---|---|---|
| `false` (기본) | wbridge 바이너리 내부 기본값만 (RT, mlock, affinity, immediate, 4MB pcap buffer) | UDP 버퍼, TX큐, IRQ pinning, RPS, coalescing, 오프로드, cpufreq, wbridge 환경변수 오버라이드 |
| `true` | 위 전부 + `optimize-for-udp.sh` + `setup-irq-affinity.sh` + `/run/wbridge.env` 생성 | — |

#### 3단계: `optimize.irq_affinity` — IRQ/RPS 제어 (`optimize.enabled=true` 시)

| `irq_affinity` | IRQ pinning | RPS | Coalescing | 오프로드 | Ring buffer |
|---|---|---|---|---|---|
| `none` | ❌ 커널 기본 | ❌ skip | ✅ 모드별 | ✅ 모드별 | ✅ 적용 |
| `auto` | 2코어+면 pinned, 1코어면 none | 동일 | ✅ 모드별 | ✅ 모드별 | ✅ 적용 |
| `pinned` | ✅ 4코어: ETH→CPU2, WLAN→CPU3 / 2코어: ETH→CPU0, WLAN→CPU1 | ✅ 동일 매핑 | ✅ 모드별 | ✅ 모드별 | ✅ 적용 |

#### 4단계: `thermal.mode_force` × `thermal_state` — Effective 모드 결정

| `mode_force` | `thermal_state` | 요청 모드 | **effective 모드** | UDP 최적화 |
|---|---|---|---|---|
| `true` (기본) | 무관 | 그대로 | **요청 모드 강제** | ✅ 실행 |
| `false` | `ok` | 그대로 | 요청 모드 | ✅ 실행 |
| `false` | `warm` | latency | **normal** | ✅ 실행 |
| `false` | `warm` | normal/eco | 그대로 | ✅ 실행 |
| `false` | `hot` | 무관 | **thermal** | ❌ skip |

> `thermal_state`는 런타임 값으로 `/run/wbridge.thermal.env`에서 주입됩니다. JSON에 설정하지 않습니다.

### 레이어별 적용 요약

```
┌─────────────────────────────────────────────────────────┐
│ wbridge 바이너리 내부 (항상 적용)                         │
│  RT 스케줄링(SCHED_FIFO) + mlockall + CPU affinity      │
│  + pcap immediate(1ms) + 4MB buffer + IP filter         │
├─────────────────────────────────────────────────────────┤
│ optimize.enabled=true 시 추가                            │
│  ┌─ optimize-for-udp.sh ───────────────────────────┐    │
│  │  TX큐 10000, 소켓버퍼 16MB, netdev backlog      │    │
│  │  파워세이브 OFF, ring buffer, UDP 메모리          │    │
│  └─────────────────────────────────────────────────┘    │
│  ┌─ setup-irq-affinity.sh (모드별) ────────────────┐    │
│  │  IRQ pinning, RPS, coalescing, 오프로드          │    │
│  │  cpufreq governor (eco/thermal), cpuidle         │    │
│  └─────────────────────────────────────────────────┘    │
│  ┌─ /run/wbridge.env (모드별 wbridge 파라미터) ────┐    │
│  │  DISPATCH_BUDGET, IMMEDIATE, TIMEOUT_MS          │    │
│  │  RT_PRIORITY, PCAP_BUFFER, TPACKET_RETIRE_TOV    │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

---

## 모드별 비교표

### setup-irq-affinity.sh (ethtool)

| 항목 | latency | normal | eco | thermal |
|---|---|---|---|---|
| **rx-usecs** | 0 | 50 | 100 | 150 |
| **tx-usecs** | 0 | 50 | 100 | 150 |
| **rx-frames** | 1 | 4 | 6 | 10 |
| **GRO** | off | on | on | on |
| **GSO** | off | off | off | off |
| **TSO** | off | off | off | off |
| **cpufreq** | — | — | conservative | powersave |
| **cpuidle deep** | — | — | — | 전부 활성화 |
| **인터럽트 빈도** (100Mbps) | ~8,000/s | ~2,000/s | ~1,000/s | ~200/s |
| **추가 레이턴시** | 0us | ~50us | ~100us | ~150us |
| **발열** | 높음 | 중간 | 낮음 | 최소 |

### wbridge 환경변수

#### wbridge-pcap용 (libpcap 기반)

| 환경변수 | latency | normal (기본값) | eco | thermal | 설명 |
|---|---|---|---|---|---|
| `WBRIDGE_DISPATCH_BUDGET` | 64 | 64 | **96** | **128** | pcap_dispatch 1회당 최대 패킷 수 |
| `WBRIDGE_IMMEDIATE` | 1 | 1 | **0** | **0** | pcap immediate mode (패킷 도착 즉시 전달) |
| `WBRIDGE_TIMEOUT_MS` | 1 | 1 | **5** | **10** | pcap 폴링 타임아웃 (ms) |
| `WBRIDGE_RT_PRIORITY` | **49** | **45** | **40** | **30** | SCHED_FIFO 우선순위 (1~99) |
| `WBRIDGE_PCAP_BUFFER` | 4194304 | 4194304 | 4194304 | **8388608** | pcap RX 버퍼 크기 (bytes) |

#### wbridge-tpacket용 (TPACKET_V3 기반)

| 환경변수 | latency | normal (기본값) | eco | thermal | 설명 |
|---|---|---|---|---|---|
| `WBRIDGE_TPACKET_RETIRE_TOV` | 1 | 1 | **5** | **10** | Block retire timeout (ms) |
| `WBRIDGE_TPACKET_BLOCK_SIZE` | **8192** | 16384 | **32768** | **65536** | RX ring block 크기 (bytes, page-aligned). 작을수록 idle stall 감소 |
| `WBRIDGE_TPACKET_BLOCK_NR` | **32** | 64 | 64 | **128** | RX ring block 개수. burst 흡수용, latency 무관 |
| `WBRIDGE_TPACKET_POLL_TIMEOUT_MS` | 1 | 1 | **0** | **0** | poll() timeout (ms). 0=auto(retire_tov×3). eco/thermal에서 빈 wake 회피 |
| `WBRIDGE_RT_PRIORITY` | **49** | **45** | **40** | **30** | SCHED_FIFO 우선순위 (1~99) |

> **참고:** TPACKET_V3 RX는 mmap zero-copy. 실시간성은 **block 크기 × retire_blk_tov**의 곱으로 결정됨 (block이 다 차거나 timeout 만료 시 user-space로 retire). `BLOCK_SIZE × BLOCK_NR`이 RX ring 총량으로, 작은 패킷의 idle traffic에서는 block이 안 차서 retire_tov 시간만큼 강제 stall 누적 → BLOCK_SIZE 축소가 latency 개선의 핵심 노브. TX_RING(TPACKET_V2)은 별도 매크로로 컴파일타임 고정(8MB).

### 왜 달라지는가

| 환경변수 | latency에서 | eco에서 | thermal에서 |
|---|---|---|---|
| **DISPATCH_BUDGET=64** | 적은 양을 자주 처리 → 지연 최소 | - | - |
| **DISPATCH_BUDGET=96/128** | - | eco: 중간 배치 | thermal: 큰 배치 → wakeup 횟수 최소 |
| **IMMEDIATE=1** | 패킷 도착 즉시 pcap에서 반환 | - | - |
| **IMMEDIATE=0** | - | timeout까지 대기 후 배치 반환 | CPU idle 확보 극대화 |
| **TIMEOUT_MS=1** | 1ms마다 깨어남 → 빠른 반응 | - | - |
| **TIMEOUT_MS=5/10** | - | eco: 5ms 간격 | thermal: 10ms → C-state 진입 가능 |
| **RT_PRIORITY=49** | 높은 우선순위 → 선점 스케줄링 유리 | - | - |
| **RT_PRIORITY=40/30** | - | eco: 중간 | thermal: 낮음 → 스케줄러 부하 감소 |
| **PCAP_BUFFER=8MB** | - | - | 병합으로 burst 도착 → 큰 버퍼로 드롭 방지 |
| **cpufreq=conservative** | - | up=80/down=20, 필요할 때만 클럭 상승 | - |
| **cpufreq=powersave** | - | - | 최저 클럭 고정 + deep idle 활성화 |

### 공통 설정 (모드 무관)

| 항목 | 값 | 설명 |
|---|---|---|
| IRQ Affinity ETH | CPU 2 (4코어 pinned 시) | `echo 4 > /proc/irq/<N>/smp_affinity` |
| IRQ Affinity WLAN | CPU 3 (4코어 pinned 시) | `echo 8 > /proc/irq/<N>/smp_affinity` |
| RPS ETH | CPU 2 (4코어 pinned 시) | `echo 4 > /sys/class/net/eth0/queues/rx-0/rps_cpus` |
| RPS WLAN | CPU 3 (4코어 pinned 시) | `echo 8 > /sys/class/net/mlan0/queues/rx-0/rps_cpus` |
| Ring Buffer | rx:4096 tx:4096 | `ethtool -G <if> rx 4096 tx 4096` |
| RX/TX Checksum | on | `ethtool -K <if> rx on tx on` |
| WBRIDGE_AFFINITY | 1 | Thread 0→CPU 0, Thread 1→CPU 1 |
| WBRIDGE_RT | 1 | SCHED_FIFO 활성 |
| WBRIDGE_MLOCK | 1 | 메모리 잠금 (page fault 방지) |
| WBRIDGE_SNAPLEN | 1600 | 패킷 캡처 길이 |
| WBRIDGE_PROMISC | 1 | 무차별 모드 |

> **참고:** IRQ Affinity, RPS는 `irq_affinity=pinned` (또는 `auto`+2코어 이상) 일 때만 적용됩니다. `irq_affinity=none`이면 커널 기본 분배를 사용합니다.

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
WBRIDGE_RT_PRIORITY=49 \
  wbridge eth0 mlan0
```

**용도:** 실시간 제어, 지연에 민감한 트래픽
**특성:**
- 패킷 도착 즉시 인터럽트 (rx-usecs=0)
- pcap immediate mode ON (패킷 즉시 전달)
- GRO OFF (패킷 병합 없이 개별 처리)
- RT 우선순위 49 (IRQ thread 50 바로 아래 — IRQ 양보, user-space RT 유지)
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

### eco 모드 (저전력)

```bash
# 1. 네트워크 설정
sudo ./setup-irq-affinity.sh --mode eco eth0 mlan0

# 2. wbridge 실행
WBRIDGE_DISPATCH_BUDGET=96 \
WBRIDGE_IMMEDIATE=0 \
WBRIDGE_TIMEOUT_MS=5 \
WBRIDGE_RT_PRIORITY=40 \
  wbridge eth0 mlan0
```

**용도:** 온도 저감 + 레이턴시 유지가 필요한 환경
**특성:**
- 100us 병합 또는 6패킷마다 인터럽트
- immediate mode OFF (5ms 배치 전달)
- cpufreq conservative (필요할 때만 클럭 상승, up=80/down=20)
- RT 우선순위 40 (normal보다 약간 낮음)
- thermal보다 레이턴시 양호 (~100us), normal보다 발열 낮음

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
WBRIDGE_TPACKET_BLOCK_SIZE=65536 \
WBRIDGE_TPACKET_BLOCK_NR=128 \
WBRIDGE_TPACKET_POLL_TIMEOUT_MS=10 \
WBRIDGE_RT_PRIORITY=30 \
  wbridge-tpacket eth0 mlan0
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
WBRIDGE_IMMEDIATE=1 WBRIDGE_TIMEOUT_MS=1 WBRIDGE_RT_PRIORITY=49 \
  systemctl start wifi_bridge@mlan0
```

## 정책 메타데이터 키

`setup-irq-affinity.sh`가 `/run/wbridge.env`에 아래 키를 기록합니다.

| 키 | 의미 |
|---|---|
| `WBRIDGE_PROFILE_VERSION` | 프로파일 스키마 버전 |
| `WBRIDGE_MODE_REQUESTED` | 요청 모드 (`latency`/`normal`/`eco`/`thermal`) |
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

`wifi_init_conf.json`의 `wbridge` 섹션이 **SSoT(Single Source of Truth)** 입니다.
`/etc/default/wbridge`는 JSON이 없거나 파싱 실패 시에만 사용되는 **fallback** 입니다.

**우선순위**: `wifi_init_conf.json` > `/etc/default/wbridge` > 스크립트 기본값

```json
// wifi_init_conf.json (SSoT)
"wbridge": {
    "enabled": true,
    "engine": "pcap",
    "optimize": {
        "enabled": true,
        "mode": "normal",
        "irq_affinity": "auto",
        "profile_version": 1
    },
    "thermal": {
        "mode_force": false
    }
}
```

환경변수 폴백 (`/etc/default/wbridge`, JSON 복구 불가 시에만 사용):

```bash
WBRIDGE_OPTIMIZE=1
WBRIDGE_MODE=normal
WBRIDGE_ENGINE=pcap
WBRIDGE_IRQ_AFFINITY=auto
WBRIDGE_MODE_FORCE=0
```

thermal effective 모드에서는 공격적 UDP 튜닝(`optimize-for-udp.sh`)이 자동 스킵됩니다.
단, `thermal.mode_force=true` (또는 `WBRIDGE_MODE_FORCE=1`)이면 thermal에서도 강제 실행됩니다.

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
journalctl -t wbridge-tpacket -n 50 --no-pager
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
