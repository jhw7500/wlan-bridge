#!/bin/bash
# IRQ Affinity 설정 스크립트
# i.MX8MM + NXP88W9098 무선 칩 환경 최적화

set -e

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== IRQ Affinity 최적화 스크립트 ===${NC}"
echo ""

# Root 권한 확인
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}ERROR: 이 스크립트는 root 권한이 필요합니다.${NC}"
    echo "사용법: sudo $0 <eth_interface> <wlan_interface>"
    exit 1
fi

# 인자 확인
if [ $# -ne 2 ]; then
    echo "사용법: $0 <eth_interface> <wlan_interface>"
    echo "예: $0 eth0 wlan0"
    exit 1
fi

ETH_IF=$1
WLAN_IF=$2

echo "유선 인터페이스: $ETH_IF"
echo "무선 인터페이스: $WLAN_IF"
echo ""

# CPU 코어 수 확인
CPU_COUNT=$(nproc)
echo "사용 가능한 CPU 코어: $CPU_COUNT"

if [ $CPU_COUNT -lt 2 ]; then
    echo -e "${YELLOW}WARNING: CPU 코어가 2개 미만입니다. 최적화 효과가 제한적일 수 있습니다.${NC}"
fi

# IRQ 찾기 함수
find_irq() {
    local iface=$1
    local irq=$(cat /proc/interrupts | grep -i "$iface" | awk '{print $1}' | tr -d ':' | head -1)
    echo "$irq"
}

# Ethernet IRQ 설정
ETH_IRQ=$(find_irq "$ETH_IF")
if [ -z "$ETH_IRQ" ]; then
    echo -e "${YELLOW}WARNING: $ETH_IF의 IRQ를 찾을 수 없습니다.${NC}"
else
    echo -e "${GREEN}$ETH_IF IRQ: $ETH_IRQ${NC}"
    # CPU 2에 할당 (브릿지 스레드는 CPU 0, 1 사용)
    if [ $CPU_COUNT -gt 2 ]; then
        echo 4 > /proc/irq/$ETH_IRQ/smp_affinity_list 2>/dev/null || echo -e "${YELLOW}WARNING: IRQ $ETH_IRQ affinity 설정 실패${NC}"
        echo "  → CPU 2에 고정"
    else
        echo -e "${YELLOW}  → CPU 2가 없어 affinity 설정 건너뜀${NC}"
    fi
fi

# Wireless IRQ 설정
WLAN_IRQ=$(find_irq "$WLAN_IF")
if [ -z "$WLAN_IRQ" ]; then
    # mlan0 등 다른 이름 시도
    WLAN_IRQ=$(find_irq "mlan")
fi

if [ -z "$WLAN_IRQ" ]; then
    echo -e "${YELLOW}WARNING: $WLAN_IF의 IRQ를 찾을 수 없습니다.${NC}"
else
    echo -e "${GREEN}$WLAN_IF IRQ: $WLAN_IRQ${NC}"
    # CPU 3에 할당
    if [ $CPU_COUNT -gt 3 ]; then
        echo 8 > /proc/irq/$WLAN_IRQ/smp_affinity_list 2>/dev/null || echo -e "${YELLOW}WARNING: IRQ $WLAN_IRQ affinity 설정 실패${NC}"
        echo "  → CPU 3에 고정"
    else
        echo -e "${YELLOW}  → CPU 3이 없어 affinity 설정 건너뜀${NC}"
    fi
fi

echo ""

# RPS/RFS 설정 (Receive Packet Steering)
echo -e "${GREEN}=== RPS/RFS 설정 ===${NC}"

# Ethernet RPS
if [ -d "/sys/class/net/$ETH_IF/queues/rx-0" ]; then
    echo 4 > /sys/class/net/$ETH_IF/queues/rx-0/rps_cpus 2>/dev/null && echo "$ETH_IF RPS → CPU 2" || echo -e "${YELLOW}$ETH_IF RPS 설정 실패${NC}"
fi

# Wireless RPS
if [ -d "/sys/class/net/$WLAN_IF/queues/rx-0" ]; then
    echo 8 > /sys/class/net/$WLAN_IF/queues/rx-0/rps_cpus 2>/dev/null && echo "$WLAN_IF RPS → CPU 3" || echo -e "${YELLOW}$WLAN_IF RPS 설정 실패${NC}"
fi

echo ""

# Ring buffer 크기 조정 (ethtool 사용)
echo -e "${GREEN}=== Ring Buffer 크기 조정 ===${NC}"

if command -v ethtool &> /dev/null; then
    # Ethernet
    ethtool -G $ETH_IF rx 4096 tx 4096 2>/dev/null && echo "$ETH_IF ring buffer → 4096" || echo -e "${YELLOW}$ETH_IF ring buffer 조정 실패 (드라이버 미지원 가능)${NC}"

    # Wireless
    ethtool -G $WLAN_IF rx 4096 tx 4096 2>/dev/null && echo "$WLAN_IF ring buffer → 4096" || echo -e "${YELLOW}$WLAN_IF ring buffer 조정 실패 (드라이버 미지원 가능)${NC}"
else
    echo -e "${YELLOW}ethtool이 설치되지 않아 ring buffer 조정을 건너뜁니다.${NC}"
    echo "설치: sudo apt-get install ethtool"
fi

echo ""

# Interrupt Coalescing 설정 (레이턴시 최소화)
echo -e "${GREEN}=== Interrupt Coalescing 설정 (레이턴시 최소화) ===${NC}"

if command -v ethtool &> /dev/null; then
    # Ethernet - 레이턴시 우선
    ethtool -C $ETH_IF rx-usecs 0 rx-frames 1 2>/dev/null && echo "$ETH_IF coalescing → 최소 레이턴시" || echo -e "${YELLOW}$ETH_IF coalescing 설정 실패${NC}"

    # Wireless - 레이턴시 우선
    ethtool -C $WLAN_IF rx-usecs 0 rx-frames 1 2>/dev/null && echo "$WLAN_IF coalescing → 최소 레이턴시" || echo -e "${YELLOW}$WLAN_IF coalescing 설정 실패${NC}"
fi

echo ""

# 하드웨어 오프로드 설정
echo -e "${GREEN}=== 하드웨어 오프로드 설정 ===${NC}"

if command -v ethtool &> /dev/null; then
    echo "레이턴시 우선 설정 (GRO/GSO/TSO 비활성화):"

    # Ethernet
    ethtool -K $ETH_IF gro off gso off tso off 2>/dev/null && echo "  $ETH_IF: GRO/GSO/TSO OFF" || echo -e "${YELLOW}  $ETH_IF 오프로드 설정 실패${NC}"
    ethtool -K $ETH_IF rx on tx on 2>/dev/null && echo "  $ETH_IF: RX/TX checksum ON" || true

    # Wireless
    ethtool -K $WLAN_IF gro off gso off tso off 2>/dev/null && echo "  $WLAN_IF: GRO/GSO/TSO OFF" || echo -e "${YELLOW}  $WLAN_IF 오프로드 설정 실패${NC}"
    ethtool -K $WLAN_IF rx on tx on 2>/dev/null && echo "  $WLAN_IF: RX/TX checksum ON" || true

    echo ""
    echo -e "${YELLOW}참고: 처리량 우선이면 다음 명령 사용:${NC}"
    echo "  ethtool -K $ETH_IF gro on gso on tso on"
    echo "  ethtool -K $WLAN_IF gro on gso on tso on"
fi

echo ""

# 설정 확인
echo -e "${GREEN}=== 현재 설정 확인 ===${NC}"
echo ""
echo "IRQ Affinity:"
if [ -n "$ETH_IRQ" ]; then
    cat /proc/irq/$ETH_IRQ/smp_affinity_list 2>/dev/null && echo "  $ETH_IF (IRQ $ETH_IRQ) → CPU $(cat /proc/irq/$ETH_IRQ/smp_affinity_list 2>/dev/null)" || true
fi
if [ -n "$WLAN_IRQ" ]; then
    cat /proc/irq/$WLAN_IRQ/smp_affinity_list 2>/dev/null && echo "  $WLAN_IF (IRQ $WLAN_IRQ) → CPU $(cat /proc/irq/$WLAN_IRQ/smp_affinity_list 2>/dev/null)" || true
fi

echo ""
echo -e "${GREEN}설정 완료!${NC}"
echo ""
echo "브릿지 실행 시 다음 CPU 사용을 권장합니다:"
echo "  Thread 0 (${ETH_IF}) → CPU 0"
echo "  Thread 1 (${WLAN_IF}) → CPU 1"
echo "  ${ETH_IF} IRQ → CPU 2"
echo "  ${WLAN_IF} IRQ → CPU 3"
echo ""
echo "재부팅 시 이 설정은 초기화됩니다."
echo "영구 적용하려면 /etc/rc.local 또는 systemd 서비스에 추가하세요."
