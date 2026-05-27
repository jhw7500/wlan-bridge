#!/bin/bash
# IRQ Affinity 및 네트워크 최적화 스크립트
# i.MX8MM + NXP88W9098 무선 칩 환경
#
# 사용법: sudo ./setup-irq-affinity.sh [--mode MODE] <eth_if> <wlan_if>
#   MODE: latency | thermal | normal (기본: normal)
#
# wbridge 환경변수를 /run/wbridge.env에 저장합니다.
# wifi_bridge.sh 또는 수동 실행 시 자동으로 source됩니다.

set -e

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

TAG="setup-irq-affinity"
ENV_FILE="/run/wbridge.env"

# ─── 로깅 ───
# BASH_LINENO[0]는 함수 호출자의 라인 번호 → C 코드의 __LINE__ 와 동일 형식.
log_info()  { local ln=${BASH_LINENO[0]}; logger -p local0.info  "[$TAG:$ln] $*"; echo -e "${GREEN}$*${NC}"; }
log_warn()  { local ln=${BASH_LINENO[0]}; logger -p local0.warn  "[$TAG:$ln] $*"; echo -e "${YELLOW}$*${NC}"; }
log_err()   { local ln=${BASH_LINENO[0]}; logger -p local0.err   "[$TAG:$ln] $*"; echo -e "${RED}$*${NC}"; }

# ─── 기본값 ───
MODE="normal"
BUS_TYPE="${WBRIDGE_BUS_TYPE:-pcie}"
PROFILE_VERSION="${WBRIDGE_PROFILE_VERSION:-1}"
THERMAL_STATE="${WBRIDGE_THERMAL_STATE:-unknown}"
MODE_FORCE="${WBRIDGE_MODE_FORCE:-0}"

# ─── 인자 파싱 ───
while [[ $# -gt 0 ]]; do
    case $1 in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --bus-type)
            BUS_TYPE="$2"
            shift 2
            ;;
        -h|--help)
            echo "사용법: $0 [--mode MODE] [--bus-type TYPE] <eth_interface> <wlan_interface>"
            echo ""
            echo "MODE:"
            echo "  latency  - 레이턴시 최소화 (인터럽트 즉시 처리, GRO OFF)"
            echo "  eco      - 저전력 모드 (온도 저감 + 레이턴시 유지)"
            echo "  thermal  - 발열 최소화 (인터럽트 병합, GRO ON)"
            echo "  normal   - 균형 모드 (기본값)"
            echo ""
            echo "BUS TYPE:"
            echo "  pcie     - PCIe 버스 (imx8mm, 기본값)"
            echo "  sdio     - SDIO 버스 (imx93, WLAN IRQ를 mmc2에서 검색)"
            echo ""
            echo "예:"
            echo "  $0 eth0 mlan0                              # normal/pcie"
            echo "  $0 --mode latency eth0 mlan0               # 레이턴시 우선"
            echo "  $0 --mode normal --bus-type sdio eth0 mlan0 # imx93 SDIO"
            echo ""
            echo "wbridge 환경변수는 $ENV_FILE 에 저장됩니다."
            exit 0
            ;;
        *)
            if [ -z "$ETH_IF" ]; then
                ETH_IF="$1"
            elif [ -z "$WLAN_IF" ]; then
                WLAN_IF="$1"
            fi
            shift
            ;;
    esac
done

# Root 권한 확인
if [ "$EUID" -ne 0 ]; then
    log_err "ERROR: root 권한이 필요합니다."
    exit 1
fi

# 인자 확인
if [ -z "$ETH_IF" ] || [ -z "$WLAN_IF" ]; then
    echo "사용법: $0 [--mode MODE] <eth_interface> <wlan_interface>"
    exit 1
fi

# 모드 검증
case "$MODE" in
    latency|thermal|normal|eco) ;;
    *)
        log_err "ERROR: 알 수 없는 모드 '$MODE' (latency|normal|eco|thermal)"
        exit 1
        ;;
esac

MODE_REQUESTED="${WBRIDGE_MODE_REQUESTED:-$MODE}"
PROFILE_EFFECTIVE="$MODE"

case "$MODE_REQUESTED" in
    latency|thermal|normal|eco) ;;
    *)
        log_warn "WARNING: 알 수 없는 요청 모드 '$MODE_REQUESTED', effective 모드($MODE)로 대체"
        MODE_REQUESTED="$MODE"
        ;;
esac

case "$THERMAL_STATE" in
    ok|warm|hot|unknown) ;;
    *)
        log_warn "WARNING: 알 수 없는 thermal state '$THERMAL_STATE', unknown으로 대체"
        THERMAL_STATE="unknown"
        ;;
esac

case "$MODE_FORCE" in
    0|1) ;;
    *)
        log_warn "WARNING: 알 수 없는 mode force '$MODE_FORCE', 0으로 대체"
        MODE_FORCE="0"
        ;;
esac

# ─── 모드별 파라미터 정의 ───
case "$MODE" in
    latency)
        MODE_DESC="레이턴시 최소화"
        RX_USECS=0;   TX_USECS=0;   RX_FRAMES=1
        GRO=off;      GSO=off;      TSO=off
        # wbridge-pcap 환경변수
        WB_DISPATCH_BUDGET=64
        WB_IMMEDIATE=1
        WB_TIMEOUT_MS=1
        # RT 49: IRQ thread(mmc2/stmmac/moal_br_*)와 moal bridge worker가 모두
        # RT=50이라 그 위 80이면 SDIO/eth IRQ thread preempt → 자기 자신 bottleneck.
        # 49는 50 미만으로 IRQ에 양보하면서 user-space RT 성격은 유지.
        WB_RT_PRIORITY=49
        WB_PCAP_BUFFER=4194304
        # wbridge-tpacket 환경변수
        # 작은 block(8KB) + 작은 ring(256KB) + poll=1ms로 idle stall 제거.
        WB_TPACKET_RETIRE_TOV=1
        WB_TPACKET_BLOCK_SIZE=8192
        WB_TPACKET_BLOCK_NR=32
        WB_TPACKET_POLL_TIMEOUT_MS=1
        ;;
    thermal)
        MODE_DESC="발열 최소화"
        RX_USECS=150; TX_USECS=150; RX_FRAMES=10
        # GRO off: mlan0(SDIO 88W9098) ndo_set_features 미등록 → mlan0 측 효과 0,
        # eth0(FEC)에서 GRO 머지 비활성으로 작은 패킷 head latency ↓ + bridge 포워딩
        # 머지/재분할 비용 회피. 4모드 일관화 (latency가 이미 off).
        GRO=off;      GSO=off;      TSO=off
        # wbridge-pcap 환경변수
        WB_DISPATCH_BUDGET=128
        WB_IMMEDIATE=0
        WB_TIMEOUT_MS=10
        WB_RT_PRIORITY=30
        WB_PCAP_BUFFER=8388608
        # wbridge-tpacket 환경변수 (발열 최적화)
        # 큰 block(64KB) + 큰 ring(8MB), POLL=0(auto, retire_tov*3=30ms)로
        # 빈 wake 최소화 → C-state 진입 시간 극대화.
        WB_TPACKET_RETIRE_TOV=10
        WB_TPACKET_BLOCK_SIZE=65536
        WB_TPACKET_BLOCK_NR=128
        WB_TPACKET_POLL_TIMEOUT_MS=0
        ;;
    eco)
        MODE_DESC="저전력 (온도 저감 우선, 레이턴시 약간 양보)"
        RX_USECS=100; TX_USECS=100; RX_FRAMES=6
        GRO=off;      GSO=off;      TSO=off
        # wbridge-pcap 환경변수
        WB_DISPATCH_BUDGET=96
        WB_IMMEDIATE=0
        WB_TIMEOUT_MS=5
        WB_RT_PRIORITY=40
        WB_PCAP_BUFFER=4194304
        # wbridge-tpacket 환경변수
        # 중간 block(32KB) + 2MB ring, POLL=0(auto, retire_tov*3=15ms) →
        # poll < retire로 인한 빈 wake 회피 (eco 발열 절감 의도 정합).
        WB_TPACKET_RETIRE_TOV=5
        WB_TPACKET_BLOCK_SIZE=32768
        WB_TPACKET_BLOCK_NR=64
        WB_TPACKET_POLL_TIMEOUT_MS=0
        ;;
    normal)
        MODE_DESC="균형 (일반)"
        RX_USECS=50;  TX_USECS=50;  RX_FRAMES=4
        GRO=off;      GSO=off;      TSO=off
        # wbridge-pcap 환경변수 (기본값)
        WB_DISPATCH_BUDGET=64
        WB_IMMEDIATE=1
        WB_TIMEOUT_MS=1
        # RT 45: IRQ thread/moal_br_* (RT=50)와 동등하지 않게 5단계 양보.
        # latency(49) > normal(45) > eco(40) > thermal(30) 단조 시퀀스.
        WB_RT_PRIORITY=45
        WB_PCAP_BUFFER=4194304
        # wbridge-tpacket 환경변수
        # 16KB block + 1MB ring + poll=1ms.
        WB_TPACKET_RETIRE_TOV=1
        WB_TPACKET_BLOCK_SIZE=16384
        WB_TPACKET_BLOCK_NR=64
        WB_TPACKET_POLL_TIMEOUT_MS=1
        ;;
esac

log_info "=== IRQ/네트워크 최적화 시작 [mode=$MODE, $MODE_DESC, bus=$BUS_TYPE] ==="
log_info "유선: $ETH_IF / 무선: $WLAN_IF"

# ─── CPU 코어 수 확인 ───
CPU_COUNT=$(nproc)
log_info "CPU 코어: $CPU_COUNT"
if [ $CPU_COUNT -lt 2 ]; then
    log_warn "WARNING: 코어 2개 미만, 효과 제한적"
fi

# ─── IRQ Affinity 정책 결정 ───
# auto: 코어 수에 따라 자동 결정, pinned: 명시적 CPU 핀, none: 커널 기본
IRQ_AFFINITY="${WBRIDGE_IRQ_AFFINITY:-auto}"
case "$IRQ_AFFINITY" in
    auto|pinned|none) ;;
    *)
        log_warn "WARNING: 알 수 없는 IRQ affinity '$IRQ_AFFINITY', auto로 대체"
        IRQ_AFFINITY="auto"
        ;;
esac

if [ "$IRQ_AFFINITY" = "auto" ]; then
    if [ $CPU_COUNT -ge 2 ]; then
        IRQ_AFFINITY="pinned"
    else
        IRQ_AFFINITY="none"
    fi
    log_info "IRQ affinity auto → $IRQ_AFFINITY (${CPU_COUNT}코어)"
fi

# ─── IRQ Affinity CPU 매핑 결정 ───
# 4코어+: ETH→CPU2, WLAN→CPU3 (IRQ 전용 코어 분리)
# 2-3코어: ETH→CPU0, WLAN→CPU1 (브릿지 스레드와 같은 코어 공유, 캐시 히트)
if [ "$IRQ_AFFINITY" = "pinned" ]; then
    if [ $CPU_COUNT -ge 4 ]; then
        ETH_AFFINITY_HEX=4;  ETH_AFFINITY_CPU=2
        WLAN_AFFINITY_HEX=8; WLAN_AFFINITY_CPU=3
    else
        ETH_AFFINITY_HEX=1;  ETH_AFFINITY_CPU=0
        WLAN_AFFINITY_HEX=2; WLAN_AFFINITY_CPU=1
    fi
fi

# ─── IRQ 찾기 ───
find_irq() {
    local pattern=$1
    cat /proc/interrupts | grep -i "$pattern" | awk '{print $1}' | tr -d ':' | head -1
}

# SDIO 버스의 WLAN IRQ는 인터페이스 이름이 아닌 mmc 컨트롤러로 등록됨
# imx93: mmc2 (IRQ 96)
find_wlan_irq() {
    local wlan_if=$1
    local irq=""

    if [ "$BUS_TYPE" = "sdio" ]; then
        # SDIO: mmc2에서 검색 (mmc0은 eMMC이므로 제외)
        irq=$(find_irq "mmc2")
        [ -z "$irq" ] && irq=$(find_irq "mmc1")
        [ -z "$irq" ] && irq=$(find_irq "$wlan_if")
    else
        # PCIe: 인터페이스 이름으로 검색
        irq=$(find_irq "$wlan_if")
        [ -z "$irq" ] && irq=$(find_irq "mlan")
    fi

    echo "$irq"
}

# ─── 1. IRQ Affinity ───
log_info "[1/5] IRQ Affinity [policy=$IRQ_AFFINITY, bus=$BUS_TYPE]"

if [ "$IRQ_AFFINITY" = "pinned" ]; then
    ETH_IRQ=$(find_irq "$ETH_IF")
    if [ -n "$ETH_IRQ" ]; then
        echo $ETH_AFFINITY_HEX > /proc/irq/$ETH_IRQ/smp_affinity 2>/dev/null && \
            log_info "  $ETH_IF (IRQ $ETH_IRQ) → CPU $ETH_AFFINITY_CPU" || \
            log_warn "  $ETH_IF affinity 설정 실패"
    else
        log_warn "  $ETH_IF: IRQ 미발견"
    fi

    WLAN_IRQ=$(find_wlan_irq "$WLAN_IF")
    if [ -n "$WLAN_IRQ" ]; then
        echo $WLAN_AFFINITY_HEX > /proc/irq/$WLAN_IRQ/smp_affinity 2>/dev/null && \
            log_info "  $WLAN_IF (IRQ $WLAN_IRQ, bus=$BUS_TYPE) → CPU $WLAN_AFFINITY_CPU" || \
            log_warn "  $WLAN_IF affinity 설정 실패"
    else
        log_warn "  $WLAN_IF: IRQ 미발견 (bus=$BUS_TYPE)"
    fi
else
    log_info "  IRQ affinity 설정 skip (policy=none, 커널 기본 분배)"
fi

# ─── 2. RPS ───
log_info "[2/5] RPS (Receive Packet Steering)"

if [ "$IRQ_AFFINITY" = "pinned" ]; then
    if [ -d "/sys/class/net/$ETH_IF/queues/rx-0" ]; then
        echo $ETH_AFFINITY_HEX > /sys/class/net/$ETH_IF/queues/rx-0/rps_cpus 2>/dev/null && \
            log_info "  $ETH_IF RPS → CPU $ETH_AFFINITY_CPU" || log_warn "  $ETH_IF RPS 실패"
    fi
    if [ -d "/sys/class/net/$WLAN_IF/queues/rx-0" ]; then
        echo $WLAN_AFFINITY_HEX > /sys/class/net/$WLAN_IF/queues/rx-0/rps_cpus 2>/dev/null && \
            log_info "  $WLAN_IF RPS → CPU $WLAN_AFFINITY_CPU" || log_warn "  $WLAN_IF RPS 실패"
    fi
else
    log_info "  RPS 설정 skip (policy=none)"
fi

# ─── 3. Ring Buffer (공통) ───
# 각 인터페이스의 ring/coalesce/offload 적용 결과를 ETH/WLAN별로 캡쳐하여
# /run/wbridge.env + /run/wbridge.apply.json 에 노출 (운용 가시성).
# mlan0(SDIO 88W9098)은 woal_netdev_ops에 ethtool_ops 미등록이라 unsupported로 찍힘.
# RX/TX는 hardware Pre-set maximum을 ethtool -g로 감지하여 clamp
# (예: imx93 stmmac max=1024 → 4096 요청 시 거부됨 → max로 clamp).
log_info "[3/5] Ring Buffer"

# Pre-set maximums 섹션에서 RX 또는 TX 최대값 추출. 미지원 시 빈 문자열.
get_ring_max() {
    local IF=$1 dir=$2
    ethtool -g "$IF" 2>/dev/null | \
        awk -v d="${dir}:" '/Pre-set maximums/{f=1; next} /Current hardware/{f=0} f && $1==d {print $2; exit}'
}

TARGET_RING=4096
ETHTOOL_RING_ETH="unknown"
ETHTOOL_RING_WLAN="unknown"
if command -v ethtool &> /dev/null; then
    for IF in "$ETH_IF" "$WLAN_IF"; do
        _max_rx=$(get_ring_max "$IF" RX)
        _max_tx=$(get_ring_max "$IF" TX)
        _rx=$TARGET_RING
        _tx=$TARGET_RING
        if [[ "$_max_rx" =~ ^[0-9]+$ ]] && [ "$_max_rx" -lt "$_rx" ]; then _rx=$_max_rx; fi
        if [[ "$_max_tx" =~ ^[0-9]+$ ]] && [ "$_max_tx" -lt "$_tx" ]; then _tx=$_max_tx; fi
        if ethtool -G "$IF" rx "$_rx" tx "$_tx" 2>/dev/null; then
            log_info "  $IF → rx:$_rx tx:$_tx (hw max rx:${_max_rx:-?} tx:${_max_tx:-?})"
            _result="supported"
        else
            log_warn "  $IF ring buffer 미지원 (max rx:${_max_rx:-?} tx:${_max_tx:-?})"
            _result="unsupported"
        fi
        if [ "$IF" = "$ETH_IF" ]; then
            ETHTOOL_RING_ETH="$_result"
        else
            ETHTOOL_RING_WLAN="$_result"
        fi
    done
else
    log_warn "  ethtool 미설치"
    ETHTOOL_RING_ETH="no_ethtool"
    ETHTOOL_RING_WLAN="no_ethtool"
fi

# ─── 4. Interrupt Coalescing (모드별) ───
log_info "[4/5] Interrupt Coalescing [${MODE}]"

ETHTOOL_COALESCE_ETH="unknown"
ETHTOOL_COALESCE_WLAN="unknown"
if command -v ethtool &> /dev/null; then
    for IF in "$ETH_IF" "$WLAN_IF"; do
        if [ $RX_USECS -eq 0 ]; then
            if ethtool -C "$IF" rx-usecs 0 rx-frames 1 2>/dev/null; then
                log_info "  $IF → 즉시 처리 (rx-usecs=0, rx-frames=1)"
                _result="supported"
            else
                log_warn "  $IF coalescing 미지원"
                _result="unsupported"
            fi
        else
            if ethtool -C "$IF" rx-usecs $RX_USECS tx-usecs $TX_USECS rx-frames $RX_FRAMES 2>/dev/null; then
                log_info "  $IF → 병합 (rx-usecs=$RX_USECS, rx-frames=$RX_FRAMES)"
                _result="supported"
            else
                log_warn "  $IF coalescing 미지원"
                _result="unsupported"
            fi
        fi
        if [ "$IF" = "$ETH_IF" ]; then
            ETHTOOL_COALESCE_ETH="$_result"
        else
            ETHTOOL_COALESCE_WLAN="$_result"
        fi
    done
else
    ETHTOOL_COALESCE_ETH="no_ethtool"
    ETHTOOL_COALESCE_WLAN="no_ethtool"
fi

# ─── 5. 오프로드 설정 (모드별) ───
log_info "[5/5] 오프로드 [${MODE}]"

ETHTOOL_OFFLOAD_ETH="unknown"
ETHTOOL_OFFLOAD_WLAN="unknown"
if command -v ethtool &> /dev/null; then
    for IF in "$ETH_IF" "$WLAN_IF"; do
        if ethtool -K "$IF" gro $GRO gso $GSO tso $TSO 2>/dev/null; then
            log_info "  $IF → GRO=$GRO GSO=$GSO TSO=$TSO"
            _result="supported"
        else
            log_warn "  $IF 오프로드 설정 실패"
            _result="unsupported"
        fi
        ethtool -K "$IF" rx on tx on 2>/dev/null || true
        if [ "$IF" = "$ETH_IF" ]; then
            ETHTOOL_OFFLOAD_ETH="$_result"
        else
            ETHTOOL_OFFLOAD_WLAN="$_result"
        fi
    done
else
    ETHTOOL_OFFLOAD_ETH="no_ethtool"
    ETHTOOL_OFFLOAD_WLAN="no_ethtool"
fi

# ─── cpufreq 지원 여부 감지 (모드 무관, 운용 가시성) ───
# imx93은 mainline/NXP downstream 양쪽 모두 DT에 operating-points-v2 노드
# 미정의 → cpufreq DT driver probe 실패 → /sys/.../cpufreq/scaling_governor
# 부재. eco/thermal 모드 governor 정책이 no-op. cpuidle deep state만 작동.
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    CPUFREQ_SUPPORTED="yes"
else
    CPUFREQ_SUPPORTED="no"
fi

# ─── cpufreq governor (eco/thermal 전용) ───
PREV_GOVERNOR="unchanged"
if [ "$MODE" = "eco" ] || [ "$MODE" = "thermal" ]; then
    if [ "$CPUFREQ_SUPPORTED" = "yes" ]; then
        PREV_GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
        if [ "$MODE" = "eco" ]; then
            for cpu_gov in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
                echo conservative > "$cpu_gov" 2>/dev/null || true
            done
            if [ -d /sys/devices/system/cpu/cpufreq/conservative ]; then
                echo 80 > /sys/devices/system/cpu/cpufreq/conservative/up_threshold 2>/dev/null || true
                echo 20 > /sys/devices/system/cpu/cpufreq/conservative/down_threshold 2>/dev/null || true
            fi
            log_info "[cpufreq] eco: $PREV_GOVERNOR → conservative (up=80, down=20)"
        else
            for cpu_gov in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
                echo powersave > "$cpu_gov" 2>/dev/null || true
            done
            log_info "[cpufreq] thermal: $PREV_GOVERNOR → powersave"
        fi
    else
        log_warn "[cpufreq] 미지원 (DT에 operating-points-v2 노드 없음, imx93 등) — eco/thermal cpufreq 정책 skip, cpuidle deep state만 작동"
    fi
fi

# ─── cpuidle deep state 활성화 (thermal 전용) ───
if [ "$MODE" = "thermal" ]; then
    log_info "[cpuidle] thermal: deep idle state 활성화"
    for state in /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state*/disable; do
        if [ -f "$state" ]; then
            echo 0 > "$state" 2>/dev/null || true
        fi
    done
    log_info "  모든 cpuidle state 활성화 완료"
fi

# ─── 6. wbridge 환경변수 파일 생성 ───
log_info "[ENV] wbridge 환경변수 → $ENV_FILE"

cat > "$ENV_FILE" <<EOF
# wbridge 환경변수 (setup-irq-affinity.sh --mode $MODE --bus-type $BUS_TYPE 에 의해 생성)
# 생성 시각: $(date '+%Y-%m-%d %H:%M:%S')
# 모드 정책 메타데이터
WBRIDGE_BUS_TYPE=$BUS_TYPE
WBRIDGE_PROFILE_VERSION=$PROFILE_VERSION
WBRIDGE_MODE_REQUESTED=$MODE_REQUESTED
WBRIDGE_PROFILE_EFFECTIVE=$PROFILE_EFFECTIVE
WBRIDGE_THERMAL_STATE=$THERMAL_STATE
WBRIDGE_MODE_FORCE=$MODE_FORCE
WBRIDGE_MODE=$PROFILE_EFFECTIVE
WBRIDGE_IRQ_AFFINITY=$IRQ_AFFINITY
# wbridge-pcap용
WBRIDGE_DISPATCH_BUDGET=$WB_DISPATCH_BUDGET
WBRIDGE_IMMEDIATE=$WB_IMMEDIATE
WBRIDGE_TIMEOUT_MS=$WB_TIMEOUT_MS
WBRIDGE_RT_PRIORITY=$WB_RT_PRIORITY
WBRIDGE_PCAP_BUFFER=$WB_PCAP_BUFFER
# wbridge-tpacket용
WBRIDGE_TPACKET_RETIRE_TOV=$WB_TPACKET_RETIRE_TOV
WBRIDGE_TPACKET_BLOCK_SIZE=$WB_TPACKET_BLOCK_SIZE
WBRIDGE_TPACKET_BLOCK_NR=$WB_TPACKET_BLOCK_NR
WBRIDGE_TPACKET_POLL_TIMEOUT_MS=$WB_TPACKET_POLL_TIMEOUT_MS
# ethtool 지원여부 (운용 가시성 — apply.json 노출용)
WBRIDGE_ETHTOOL_COALESCE_ETH=$ETHTOOL_COALESCE_ETH
WBRIDGE_ETHTOOL_COALESCE_WLAN=$ETHTOOL_COALESCE_WLAN
WBRIDGE_ETHTOOL_RING_ETH=$ETHTOOL_RING_ETH
WBRIDGE_ETHTOOL_RING_WLAN=$ETHTOOL_RING_WLAN
WBRIDGE_ETHTOOL_OFFLOAD_ETH=$ETHTOOL_OFFLOAD_ETH
WBRIDGE_ETHTOOL_OFFLOAD_WLAN=$ETHTOOL_OFFLOAD_WLAN
# cpufreq (eco: conservative, thermal: powersave; imx93 등 미지원 SoC에선 no-op)
WBRIDGE_CPUFREQ_SUPPORTED=$CPUFREQ_SUPPORTED
WBRIDGE_CPUFREQ_PREV_GOVERNOR=$PREV_GOVERNOR
EOF

log_info "  WBRIDGE_PROFILE_VERSION=$PROFILE_VERSION"
log_info "  WBRIDGE_MODE_REQUESTED=$MODE_REQUESTED"
log_info "  WBRIDGE_PROFILE_EFFECTIVE=$PROFILE_EFFECTIVE"
log_info "  WBRIDGE_THERMAL_STATE=$THERMAL_STATE"
log_info "  WBRIDGE_MODE_FORCE=$MODE_FORCE"
log_info "  WBRIDGE_DISPATCH_BUDGET=$WB_DISPATCH_BUDGET"
log_info "  WBRIDGE_IMMEDIATE=$WB_IMMEDIATE"
log_info "  WBRIDGE_TIMEOUT_MS=$WB_TIMEOUT_MS"
log_info "  WBRIDGE_RT_PRIORITY=$WB_RT_PRIORITY"
log_info "  WBRIDGE_PCAP_BUFFER=$WB_PCAP_BUFFER"
log_info "  WBRIDGE_TPACKET_RETIRE_TOV=$WB_TPACKET_RETIRE_TOV"
log_info "  WBRIDGE_TPACKET_BLOCK_SIZE=$WB_TPACKET_BLOCK_SIZE"
log_info "  WBRIDGE_TPACKET_BLOCK_NR=$WB_TPACKET_BLOCK_NR"
log_info "  WBRIDGE_TPACKET_POLL_TIMEOUT_MS=$WB_TPACKET_POLL_TIMEOUT_MS"
log_info "  WBRIDGE_ETHTOOL_COALESCE: eth=$ETHTOOL_COALESCE_ETH wlan=$ETHTOOL_COALESCE_WLAN"
log_info "  WBRIDGE_ETHTOOL_RING:     eth=$ETHTOOL_RING_ETH wlan=$ETHTOOL_RING_WLAN"
log_info "  WBRIDGE_ETHTOOL_OFFLOAD:  eth=$ETHTOOL_OFFLOAD_ETH wlan=$ETHTOOL_OFFLOAD_WLAN"
log_info "  WBRIDGE_CPUFREQ_SUPPORTED=$CPUFREQ_SUPPORTED"

# ─── 결과 요약 ───
log_info "=== 최적화 완료 [mode=$MODE, $MODE_DESC, bus=$BUS_TYPE] ==="
if [ "$IRQ_AFFINITY" = "pinned" ]; then
    log_info "  IRQ: $ETH_IF→CPU$ETH_AFFINITY_CPU, $WLAN_IF→CPU$WLAN_AFFINITY_CPU | Coalescing: rx-usecs=$RX_USECS, rx-frames=$RX_FRAMES | GRO=$GRO"
else
    log_info "  IRQ: kernel default | Coalescing: rx-usecs=$RX_USECS, rx-frames=$RX_FRAMES | GRO=$GRO"
fi
