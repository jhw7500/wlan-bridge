#!/bin/bash
# UDP 고속 전송을 위한 시스템 최적화 스크립트 (재시작 최적화 버전)

set -e

# Root 권한 확인
if [ "$EUID" -ne 0 ]; then
    exit 1
fi

# 인자 확인
if [ $# -ne 2 ]; then
    exit 1
fi

ETH_IF=$1
WLAN_IF=$2
TARGET_QLEN=10000
TARGET_BUF=16777216

# 1. TX 큐 증가 (이미 설정되어 있으면 스킵)
for IF in "$ETH_IF" "$WLAN_IF"; do
    CURRENT_QLEN=$(ip link show "$IF" 2>/dev/null | grep -o 'qlen [0-9]*' | awk '{print $2}' || echo 0)
    if [ -n "$CURRENT_QLEN" ] && [ "$CURRENT_QLEN" -lt "$TARGET_QLEN" ]; then
        ip link set "$IF" txqueuelen "$TARGET_QLEN" 2>/dev/null || true
    fi
done

# 2. 커널 버퍼 증가 (이미 설정되어 있으면 스킵)
CURRENT_WMEM=$(sysctl -n net.core.wmem_max 2>/dev/null || echo 0)
if [ "$CURRENT_WMEM" -lt "$TARGET_BUF" ]; then
    sysctl -w net.core.wmem_max=$TARGET_BUF > /dev/null 2>&1 || true
    sysctl -w net.core.wmem_default=$TARGET_BUF > /dev/null 2>&1 || true
    sysctl -w net.core.rmem_max=$TARGET_BUF > /dev/null 2>&1 || true
    sysctl -w net.core.rmem_default=$TARGET_BUF > /dev/null 2>&1 || true
    sysctl -w net.core.netdev_max_backlog=10000 > /dev/null 2>&1 || true
fi

# 3. 무선 파워 세이빙 off
if iwconfig "$WLAN_IF" 2>/dev/null | grep -q "Power Management:on"; then
    iwconfig "$WLAN_IF" power off 2>/dev/null || true
fi

# 4. Ring buffer 증가 (ethtool 지원 시에만)
if command -v ethtool &> /dev/null; then
    # eth0는 아까 실패했으므로 에러 무시하고 시도
    ethtool -G "$ETH_IF" rx 4096 tx 4096 > /dev/null 2>&1 || true
    ethtool -G "$WLAN_IF" rx 4096 tx 4096 > /dev/null 2>&1 || true
fi

# 5. UDP 특화 설정
CURRENT_UDP_WMIN=$(sysctl -n net.ipv4.udp_wmem_min 2>/dev/null || echo 0)
if [ "$CURRENT_UDP_WMIN" -lt 16384 ]; then
    sysctl -w net.ipv4.udp_mem="8388608 12582912 16777216" > /dev/null 2>&1 || true
    sysctl -w net.ipv4.udp_rmem_min=16384 > /dev/null 2>&1 || true
    sysctl -w net.ipv4.udp_wmem_min=16384 > /dev/null 2>&1 || true
fi
