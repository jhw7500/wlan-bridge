#!/bin/bash
# UDP 고속 전송을 위한 시스템 최적화 스크립트

set -e

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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

echo -e "${GREEN}=== UDP 고속 전송을 위한 시스템 최적화 ===${NC}"
echo ""
echo "유선 인터페이스: $ETH_IF"
echo "무선 인터페이스: $WLAN_IF"
echo ""

# 1. TX 큐 증가
echo -e "${BLUE}1. TX 큐 크기 증가 (1000 → 10000)${NC}"
ip link set $ETH_IF txqueuelen 10000
ip link set $WLAN_IF txqueuelen 10000
echo "  ✓ $ETH_IF: $(ip link show $ETH_IF | grep -o 'qlen [0-9]*')"
echo "  ✓ $WLAN_IF: $(ip link show $WLAN_IF | grep -o 'qlen [0-9]*')"

# 2. 커널 버퍼 증가
echo ""
echo -e "${BLUE}2. 커널 네트워크 버퍼 증가 (기본값 → 16MB)${NC}"
sysctl -w net.core.wmem_max=16777216 > /dev/null
sysctl -w net.core.wmem_default=16777216 > /dev/null
sysctl -w net.core.rmem_max=16777216 > /dev/null
sysctl -w net.core.rmem_default=16777216 > /dev/null
sysctl -w net.core.netdev_max_backlog=10000 > /dev/null
echo "  ✓ wmem_max: $(sysctl -n net.core.wmem_max) bytes (16MB)"
echo "  ✓ rmem_max: $(sysctl -n net.core.rmem_max) bytes (16MB)"
echo "  ✓ netdev_max_backlog: $(sysctl -n net.core.netdev_max_backlog)"

# 3. 무선 파워 세이빙 off
echo ""
echo -e "${BLUE}3. 무선 파워 세이빙 비활성화${NC}"
iwconfig $WLAN_IF power off 2>/dev/null && \
    echo "  ✓ $WLAN_IF: Power Management OFF" || \
    echo -e "  ${YELLOW}⚠ $WLAN_IF: 파워 관리 설정 실패 (드라이버 미지원 가능)${NC}"

# 4. Ring buffer 증가
echo ""
echo -e "${BLUE}4. Ring buffer 크기 증가${NC}"

if command -v ethtool &> /dev/null; then
    # Ethernet
    ethtool -G $ETH_IF rx 4096 tx 4096 2>/dev/null && \
        echo "  ✓ $ETH_IF: RX/TX ring buffer → 4096" || \
        echo -e "  ${YELLOW}⚠ $ETH_IF: Ring buffer 조정 실패 (드라이버 미지원 가능)${NC}"

    # Wireless
    ethtool -G $WLAN_IF rx 4096 tx 4096 2>/dev/null && \
        echo "  ✓ $WLAN_IF: RX/TX ring buffer → 4096" || \
        echo -e "  ${YELLOW}⚠ $WLAN_IF: Ring buffer 조정 실패 (드라이버 미지원 가능)${NC}"
else
    echo -e "${YELLOW}ethtool이 설치되지 않아 ring buffer 조정을 건너뜁니다.${NC}"
    echo "설치: sudo apt-get install ethtool"
fi

# 5. UDP 특화 설정
echo ""
echo -e "${BLUE}5. UDP 특화 커널 설정${NC}"
sysctl -w net.ipv4.udp_mem="8388608 12582912 16777216" > /dev/null
sysctl -w net.ipv4.udp_rmem_min=16384 > /dev/null
sysctl -w net.ipv4.udp_wmem_min=16384 > /dev/null
echo "  ✓ UDP 버퍼 메모리: 8MB ~ 16MB"

# 6. 영구 설정 옵션
echo ""
echo -e "${BLUE}6. 영구 설정 (선택사항)${NC}"
read -p "재부팅 후에도 유지하시겠습니까? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "# UDP 고속 전송 최적화 ($(date))" >> /etc/sysctl.conf
    echo "net.core.wmem_max=16777216" >> /etc/sysctl.conf
    echo "net.core.wmem_default=16777216" >> /etc/sysctl.conf
    echo "net.core.rmem_max=16777216" >> /etc/sysctl.conf
    echo "net.core.rmem_default=16777216" >> /etc/sysctl.conf
    echo "net.core.netdev_max_backlog=10000" >> /etc/sysctl.conf
    echo "net.ipv4.udp_mem=8388608 12582912 16777216" >> /etc/sysctl.conf
    echo "net.ipv4.udp_rmem_min=16384" >> /etc/sysctl.conf
    echo "net.ipv4.udp_wmem_min=16384" >> /etc/sysctl.conf
    echo "  ✓ /etc/sysctl.conf 업데이트 완료"

    # rc.local에 인터페이스 설정 추가
    if [ -f /etc/rc.local ]; then
        echo "ip link set $ETH_IF txqueuelen 10000" >> /etc/rc.local
        echo "ip link set $WLAN_IF txqueuelen 10000" >> /etc/rc.local
        echo "  ✓ /etc/rc.local 업데이트 완료"
    else
        echo -e "  ${YELLOW}⚠ /etc/rc.local 없음. TX 큐 설정은 재부팅 시 초기화됨${NC}"
    fi
else
    echo "  ⓘ 재부팅 시 설정이 초기화됩니다."
fi

# 7. 현재 설정 요약
echo ""
echo -e "${GREEN}=== 최적화 완료! ===${NC}"
echo ""
echo -e "${BLUE}현재 설정:${NC}"
echo "  • TX Queue Length:"
echo "    - $ETH_IF: $(ip link show $ETH_IF | grep -o 'qlen [0-9]*')"
echo "    - $WLAN_IF: $(ip link show $WLAN_IF | grep -o 'qlen [0-9]*')"
echo "  • 커널 버퍼: wmem/rmem 16MB"
echo "  • Netdev backlog: 10000"
echo "  • UDP 버퍼: 8-16MB"
echo ""

# 8. 사용 가이드
echo -e "${GREEN}=== 다음 단계 ===${NC}"
echo ""
echo "1. 브릿지 시작 (TPACKET_V3 버전 권장):"
echo -e "   ${YELLOW}sudo ./dumb-tpacket $ETH_IF $WLAN_IF${NC}"
echo ""
echo "2. UDP 테스트 (권장 대역폭):"
echo -e "   ${YELLOW}# 50 Mbps부터 시작 (안전)${NC}"
echo "   iperf3 -c <server> -u -b 50M -t 30 -R"
echo ""
echo -e "   ${YELLOW}# 점진적으로 증가${NC}"
echo "   iperf3 -c <server> -u -b 100M -t 30 -R"
echo "   iperf3 -c <server> -u -b 150M -t 30 -R"
echo "   iperf3 -c <server> -u -b 200M -t 30 -R"
echo ""
echo "3. 브릿지 통계 확인:"
echo -e "   ${YELLOW}sudo kill -USR1 \$(pidof dumb-tpacket)${NC}"
echo ""
echo -e "${YELLOW}주의:${NC}"
echo "  • UDP는 -P 옵션 미지원 (TCP만 지원)"
echo "  • 500M은 너무 높음. 100-200M 권장"
echo "  • 에러 발생 시 대역폭을 낮추세요"
echo ""
echo -e "${GREEN}최적화된 환경에서 안정적인 테스트 되시길 바랍니다!${NC}"
