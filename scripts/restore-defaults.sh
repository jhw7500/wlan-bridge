#!/bin/bash
# restore-defaults.sh - 시스템 설정을 제공된 기본값으로 원복

set -e

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Root 권한 확인
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}ERROR: 이 스크립트는 root 권한이 필요합니다.${NC}"
    exit 1
fi

# 인자 확인
if [ $# -ne 2 ]; then
    echo "사용법: $0 <eth_interface> <wlan_interface>"
    echo "예: $0 eth0 mlan0"
    exit 1
fi

ETH_IF=$1
WLAN_IF=$2

echo -e "${GREEN}=== 시스템 설정을 기본값으로 원복합니다 ===${NC}"
echo ""

# 1. TX 큐 원복
echo -e "${BLUE}1. TX 큐 크기 원복 (10000 → 1000)${NC}"
ip link set $ETH_IF txqueuelen 1000 2>/dev/null || true
ip link set $WLAN_IF txqueuelen 1000 2>/dev/null || true
echo "  ✓ $ETH_IF: $(ip link show $ETH_IF | grep -o 'qlen [0-9]*' || echo 'N/A')"
echo "  ✓ $WLAN_IF: $(ip link show $WLAN_IF | grep -o 'qlen [0-9]*' || echo 'N/A')"

# 2. 커널 버퍼 원복 (제공해주신 12MB/1000 기준)
echo ""
echo -e "${BLUE}2. 커널 네트워크 버퍼 원복 (16MB → 12MB)${NC}"
sysctl -w net.core.wmem_max=12582912 > /dev/null
sysctl -w net.core.rmem_max=12582912 > /dev/null
sysctl -w net.core.wmem_default=12582912 > /dev/null
sysctl -w net.core.rmem_default=12582912 > /dev/null
sysctl -w net.core.netdev_max_backlog=1000 > /dev/null

sysctl -w net.ipv4.udp_mem="91692 122257 183384" > /dev/null
sysctl -w net.ipv4.udp_rmem_min=4096 > /dev/null
sysctl -w net.ipv4.udp_wmem_min=4096 > /dev/null

echo "  ✓ net.core.wmem_max: $(sysctl -n net.core.wmem_max)"
echo "  ✓ net.core.netdev_max_backlog: $(sysctl -n net.core.netdev_max_backlog)"
echo "  ✓ net.ipv4.udp_mem: $(sysctl -n net.ipv4.udp_mem)"

# 3. 무선 파워 세이빙 원복
echo ""
echo -e "${BLUE}3. 무선 파워 세이빙 활성화 (Power Management: ON)${NC}"
# 참고: 제공해주신 현재 상태가 off였으나, 일반적인 원복은 on으로 설정함
iwconfig $WLAN_IF power on 2>/dev/null && \
    echo "  ✓ $WLAN_IF: Power Management ON" || \
    echo -e "  ${YELLOW}⚠ $WLAN_IF: 설정 실패 (드라이버 미지원 가능)${NC}"

# 4. IRQ Affinity 초기화 (모든 코어에서 처리하도록 복구)
echo ""
echo -e "${BLUE}4. IRQ Affinity 초기화 (Reset to all CPUs)${NC}"
# 모든 코어(i.MX8 4코어 기준 ff) 허용
for irq_path in /proc/irq/*/smp_affinity; do
    if [ -f "$irq_path" ]; then
        echo "ff" > "$irq_path" 2>/dev/null || true
    fi
done
echo "  ✓ 인터럽트 처리 코어 제한 해제 완료"

# 5. 영구 설정 안내
echo ""
echo -e "${YELLOW}주의: /etc/sysctl.conf 또는 /etc/rc.local에 직접 추가한 설정은${NC}"
echo -e "${YELLOW}이 스크립트로 삭제되지 않습니다. 해당 파일들을 확인하여 수동으로 정리하세요.${NC}"

echo ""
echo -e "${GREEN}=== 원복 완료! ===${NC}"
