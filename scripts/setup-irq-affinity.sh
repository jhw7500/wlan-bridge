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
CYAN='\033[0;36m'
NC='\033[0m'

TAG="setup-irq-affinity"
ENV_FILE="/run/wbridge.env"

# ─── 로깅 ───
log_info()  { logger -p local0.info  "[$TAG] $*"; echo -e "${GREEN}$*${NC}"; }
log_warn()  { logger -p local0.warn  "[$TAG] $*"; echo -e "${YELLOW}$*${NC}"; }
log_err()   { logger -p local0.err   "[$TAG] $*"; echo -e "${RED}$*${NC}"; }

# ─── 기본값 ───
MODE="normal"
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
        -h|--help)
            echo "사용법: $0 [--mode MODE] <eth_interface> <wlan_interface>"
            echo ""
            echo "MODE:"
            echo "  latency  - 레이턴시 최소화 (인터럽트 즉시 처리, GRO OFF)"
            echo "  thermal  - 발열 최소화 (인터럽트 병합, GRO ON)"
            echo "  normal   - 균형 모드 (기본값)"
            echo ""
            echo "예:"
            echo "  $0 eth0 mlan0                    # normal 모드"
            echo "  $0 --mode latency eth0 mlan0     # 레이턴시 우선"
            echo "  $0 --mode thermal eth0 mlan0     # 발열 우선"
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
    latency|thermal|normal) ;;
    *)
        log_err "ERROR: 알 수 없는 모드 '$MODE' (latency|thermal|normal)"
        exit 1
        ;;
esac

MODE_REQUESTED="${WBRIDGE_MODE_REQUESTED:-$MODE}"
PROFILE_EFFECTIVE="$MODE"

case "$MODE_REQUESTED" in
    latency|thermal|normal) ;;
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
        WB_RT_PRIORITY=80
        WB_PCAP_BUFFER=4194304
        # wbridge-tpacket 환경변수
        WB_TPACKET_RETIRE_TOV=1
        ;;
    thermal)
        MODE_DESC="발열 최소화"
        RX_USECS=150; TX_USECS=150; RX_FRAMES=10
        GRO=on;       GSO=off;      TSO=off
        # wbridge-pcap 환경변수
        WB_DISPATCH_BUDGET=128
        WB_IMMEDIATE=0
        WB_TIMEOUT_MS=10
        WB_RT_PRIORITY=30
        WB_PCAP_BUFFER=8388608
        # wbridge-tpacket 환경변수 (발열 최적화)
        WB_TPACKET_RETIRE_TOV=10
        ;;
    normal)
        MODE_DESC="균형 (일반)"
        RX_USECS=50;  TX_USECS=50;  RX_FRAMES=4
        GRO=on;       GSO=off;      TSO=off
        # wbridge-pcap 환경변수 (기본값)
        WB_DISPATCH_BUDGET=64
        WB_IMMEDIATE=1
        WB_TIMEOUT_MS=1
        WB_RT_PRIORITY=50
        WB_PCAP_BUFFER=4194304
        # wbridge-tpacket 환경변수 (기본값)
        WB_TPACKET_RETIRE_TOV=1
        ;;
esac

log_info "=== IRQ/네트워크 최적화 시작 [mode=$MODE, $MODE_DESC] ==="
log_info "유선: $ETH_IF / 무선: $WLAN_IF"

# ─── CPU 코어 수 확인 ───
CPU_COUNT=$(nproc)
log_info "CPU 코어: $CPU_COUNT"
if [ $CPU_COUNT -lt 2 ]; then
    log_warn "WARNING: 코어 2개 미만, 효과 제한적"
fi

# ─── IRQ 찾기 ───
find_irq() {
    local iface=$1
    cat /proc/interrupts | grep -i "$iface" | awk '{print $1}' | tr -d ':' | head -1
}

# ─── 1. IRQ Affinity (공통) ───
log_info "[1/5] IRQ Affinity"

ETH_IRQ=$(find_irq "$ETH_IF")
if [ -n "$ETH_IRQ" ] && [ $CPU_COUNT -gt 2 ]; then
    echo 4 > /proc/irq/$ETH_IRQ/smp_affinity 2>/dev/null && \
        log_info "  $ETH_IF (IRQ $ETH_IRQ) → CPU 2" || \
        log_warn "  $ETH_IF affinity 설정 실패"
else
    log_warn "  $ETH_IF: IRQ 미발견 또는 CPU 부족"
fi

WLAN_IRQ=$(find_irq "$WLAN_IF")
[ -z "$WLAN_IRQ" ] && WLAN_IRQ=$(find_irq "mlan")
if [ -n "$WLAN_IRQ" ] && [ $CPU_COUNT -gt 3 ]; then
    echo 8 > /proc/irq/$WLAN_IRQ/smp_affinity 2>/dev/null && \
        log_info "  $WLAN_IF (IRQ $WLAN_IRQ) → CPU 3" || \
        log_warn "  $WLAN_IF affinity 설정 실패"
else
    log_warn "  $WLAN_IF: IRQ 미발견 또는 CPU 부족"
fi

# ─── 2. RPS (공통) ───
log_info "[2/5] RPS (Receive Packet Steering)"

if [ -d "/sys/class/net/$ETH_IF/queues/rx-0" ]; then
    echo 4 > /sys/class/net/$ETH_IF/queues/rx-0/rps_cpus 2>/dev/null && \
        log_info "  $ETH_IF RPS → CPU 2" || log_warn "  $ETH_IF RPS 실패"
fi
if [ -d "/sys/class/net/$WLAN_IF/queues/rx-0" ]; then
    echo 8 > /sys/class/net/$WLAN_IF/queues/rx-0/rps_cpus 2>/dev/null && \
        log_info "  $WLAN_IF RPS → CPU 3" || log_warn "  $WLAN_IF RPS 실패"
fi

# ─── 3. Ring Buffer (공통) ───
log_info "[3/5] Ring Buffer"

if command -v ethtool &> /dev/null; then
    ethtool -G $ETH_IF rx 4096 tx 4096 2>/dev/null && \
        log_info "  $ETH_IF → rx:4096 tx:4096" || \
        log_warn "  $ETH_IF ring buffer 미지원"
    ethtool -G $WLAN_IF rx 4096 tx 4096 2>/dev/null && \
        log_info "  $WLAN_IF → rx:4096 tx:4096" || \
        log_warn "  $WLAN_IF ring buffer 미지원"
else
    log_warn "  ethtool 미설치"
fi

# ─── 4. Interrupt Coalescing (모드별) ───
log_info "[4/5] Interrupt Coalescing [${MODE}]"

if command -v ethtool &> /dev/null; then
    for IF in $ETH_IF $WLAN_IF; do
        if [ $RX_USECS -eq 0 ]; then
            ethtool -C $IF rx-usecs 0 rx-frames 1 2>/dev/null && \
                log_info "  $IF → 즉시 처리 (rx-usecs=0, rx-frames=1)" || \
                log_warn "  $IF coalescing 미지원"
        else
            ethtool -C $IF rx-usecs $RX_USECS tx-usecs $TX_USECS rx-frames $RX_FRAMES 2>/dev/null && \
                log_info "  $IF → 병합 (rx-usecs=$RX_USECS, rx-frames=$RX_FRAMES)" || \
                log_warn "  $IF coalescing 미지원"
        fi
    done
fi

# ─── 5. 오프로드 설정 (모드별) ───
log_info "[5/5] 오프로드 [${MODE}]"

if command -v ethtool &> /dev/null; then
    for IF in $ETH_IF $WLAN_IF; do
        ethtool -K $IF gro $GRO gso $GSO tso $TSO 2>/dev/null && \
            log_info "  $IF → GRO=$GRO GSO=$GSO TSO=$TSO" || \
            log_warn "  $IF 오프로드 설정 실패"
        ethtool -K $IF rx on tx on 2>/dev/null || true
    done
fi

# ─── 6. wbridge 환경변수 파일 생성 ───
log_info "[ENV] wbridge 환경변수 → $ENV_FILE"

cat > "$ENV_FILE" <<EOF
# wbridge 환경변수 (setup-irq-affinity.sh --mode $MODE 에 의해 생성)
# 생성 시각: $(date '+%Y-%m-%d %H:%M:%S')
# 모드 정책 메타데이터
WBRIDGE_PROFILE_VERSION=$PROFILE_VERSION
WBRIDGE_MODE_REQUESTED=$MODE_REQUESTED
WBRIDGE_PROFILE_EFFECTIVE=$PROFILE_EFFECTIVE
WBRIDGE_THERMAL_STATE=$THERMAL_STATE
WBRIDGE_MODE_FORCE=$MODE_FORCE
WBRIDGE_MODE=$PROFILE_EFFECTIVE
# wbridge-pcap용
WBRIDGE_DISPATCH_BUDGET=$WB_DISPATCH_BUDGET
WBRIDGE_IMMEDIATE=$WB_IMMEDIATE
WBRIDGE_TIMEOUT_MS=$WB_TIMEOUT_MS
WBRIDGE_RT_PRIORITY=$WB_RT_PRIORITY
WBRIDGE_PCAP_BUFFER=$WB_PCAP_BUFFER
# wbridge-tpacket용
WBRIDGE_TPACKET_RETIRE_TOV=$WB_TPACKET_RETIRE_TOV
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

# ─── 결과 요약 ───
log_info "=== 최적화 완료 [mode=$MODE, $MODE_DESC] ==="
log_info "  IRQ: $ETH_IF→CPU2, $WLAN_IF→CPU3 | Coalescing: rx-usecs=$RX_USECS, rx-frames=$RX_FRAMES | GRO=$GRO"
