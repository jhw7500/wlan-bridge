# 버퍼 오버플로우 문제 해결 가이드

## 문제 1: UDP `-P 4 -R` 연결 안 됨

### 원인
**UDP는 `-P` (병렬 스트림) 옵션을 지원하지 않습니다!**

```bash
# ❌ 작동 안 함
iperf3 -c <server> -u -P 4 -R

# ✅ 올바른 사용법
iperf3 -c <server> -u -R
```

**iperf3 매뉴얼**:
- `-P`: TCP 전용 옵션
- UDP는 단일 스트림만 지원

### 해결 방법

```bash
# 단일 스트림으로 테스트
iperf3 -c <server> -u -b 100M -R

# 여러 스트림 원하면 수동으로 실행
iperf3 -c <server> -u -b 100M -p 5201 -R &
iperf3 -c <server> -u -b 100M -p 5202 -R &
iperf3 -c <server> -u -b 100M -p 5203 -R &
iperf3 -c <server> -u -b 100M -p 5204 -R &
```

---

## 문제 2: "No buffer space available" 에러

### 에러 분석

```
pcap_inject(1->0) failed: send: No buffer space available
           ↓    ↓
       wlan0 → eth0 방향
```

**의미**: 무선(wlan0)에서 유선(eth0)으로 패킷을 보낼 때, eth0의 **TX 큐가 꽉 참**

### 원인

1. **500 Mbps는 너무 높음**
   - 무선 → 유선 방향은 무선의 실제 처리량에 제한됨
   - NXP 88W9098 실제 처리량: 300-400 Mbps (현실적)
   - 500 Mbps 요청 → 큐 오버플로우

2. **UDP는 흐름 제어 없음**
   - TCP: 수신자가 느리면 송신자가 자동으로 느려짐
   - UDP: 무조건 전송 → 버퍼 폭주

3. **TX 큐 크기 부족**
   - 기본 TX 큐: 1000 패킷 (너무 작음)
   - 500 Mbps = ~41,666 pps (1500바이트 기준)
   - 큐가 0.024초만에 가득 참!

---

## 즉시 해결 방법

### 1. 대역폭 제한 (가장 중요!)

```bash
# ❌ 너무 높음 (500 Mbps)
iperf3 -c <server> -u -b 500M -R

# ✅ 현실적인 값 (100-200 Mbps)
iperf3 -c <server> -u -b 100M -R

# ✅ 더 안전한 값 (50 Mbps)
iperf3 -c <server> -u -b 50M -R
```

### 2. TX 큐 크기 증가

```bash
# 현재 큐 크기 확인
ip link show eth0 | grep qlen
# 출력: qlen 1000 (기본값)

# TX 큐 크기 대폭 증가
sudo ip link set eth0 txqueuelen 10000
sudo ip link set wlan0 txqueuelen 10000

# 확인
ip link show eth0 | grep qlen
# 출력: qlen 10000
```

### 3. 커널 버퍼 크기 증가

```bash
# 네트워크 버퍼 크기 증가
sudo sysctl -w net.core.wmem_max=16777216      # 16MB
sudo sysctl -w net.core.wmem_default=16777216
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.rmem_default=16777216

# netdev 백로그 증가
sudo sysctl -w net.core.netdev_max_backlog=10000

# 영구 적용
sudo tee -a /etc/sysctl.conf <<EOF
net.core.wmem_max=16777216
net.core.wmem_default=16777216
net.core.rmem_max=16777216
net.core.rmem_default=16777216
net.core.netdev_max_backlog=10000
EOF

sudo sysctl -p
```

### 4. pcap 버퍼 크기 증가

```bash
# dumb 실행 시 pcap 버퍼 크기 증가
sudo ./dumb --pcap-buffer 8388608 eth0 wlan0  # 8MB

# 또는 환경 변수
export DUMB_PCAP_BUFFER=8388608
sudo ./dumb eth0 wlan0
```

---

## 단계별 테스트

### Step 1: 낮은 대역폭부터 시작

```bash
# 1. TX 큐 증가
sudo ip link set eth0 txqueuelen 10000
sudo ip link set wlan0 txqueuelen 10000

# 2. 브릿지 시작 (버퍼 크기 증가)
sudo ./dumb --pcap-buffer 8388608 eth0 wlan0

# 3. 낮은 대역폭 테스트 (50 Mbps)
iperf3 -c <server> -u -b 50M -t 30 -R

# 4. 통계 확인
sudo kill -USR1 $(pidof dumb)

# 에러가 없으면 → ✅ 성공!
# 에러 계속 발생 → Step 2로
```

### Step 2: 점진적으로 대역폭 증가

```bash
# 50M 성공하면 100M 시도
iperf3 -c <server> -u -b 100M -t 30 -R
sudo kill -USR1 $(pidof dumb)

# 100M 성공하면 200M 시도
iperf3 -c <server> -u -b 200M -t 30 -R
sudo kill -USR1 $(pidof dumb)

# 에러 발생하는 시점의 대역폭 = 실제 한계
```

### Step 3: 최대 성능 찾기

```bash
# 예: 150M까지는 되고 200M부터 에러 발생
# → 실제 무선 처리량 한계 = ~150 Mbps

# 안전한 값: 한계의 80%
# 150 * 0.8 = 120 Mbps

iperf3 -c <server> -u -b 120M -t 60 -R
```

---

## dumb-tpacket.c 사용 (권장!)

libpcap 대신 TPACKET_V3 버전은 버퍼 관리가 훨씬 우수합니다.

```bash
# dumb-tpacket 사용 (이미 8MB 버퍼로 최적화됨)
sudo ./dumb-tpacket eth0 wlan0

# 테스트
iperf3 -c <server> -u -b 200M -t 30 -R

# 에러가 훨씬 적거나 없을 것!
```

---

## 무선 인터페이스 최적화

### 1. 무선 드라이버 TX 큐 확인

```bash
# 무선 드라이버 통계
ethtool -S wlan0 | grep -E "tx.*queue|tx.*drop|tx.*error"

# TX ring buffer 크기 확인
ethtool -g wlan0

# TX ring 증가 시도
sudo ethtool -G wlan0 tx 4096
```

### 2. 무선 파워 세이빙 비활성화

```bash
# 파워 세이빙이 처리량을 제한할 수 있음
sudo iwconfig wlan0 power off

# 확인
iwconfig wlan0 | grep "Power Management"
# 출력: Power Management:off
```

### 3. 무선 TX 파워 최대화

```bash
# 현재 TX 파워 확인
iwconfig wlan0 | grep "Tx-Power"

# 최대로 설정
sudo iwconfig wlan0 txpower 30  # dBm (법적 한계 내)
```

---

## 예상 결과

### 최적화 전

```bash
iperf3 -c <server> -u -b 500M -R

# 결과:
# - "No buffer space available" 에러 대량 발생
# - Dropped 카운터 30000+
# - 실제 처리량: 50-100 Mbps (에러 때문에)
```

### 최적화 후

```bash
# 1. 설정
sudo ip link set eth0 txqueuelen 10000
sudo ip link set wlan0 txqueuelen 10000
sudo sysctl -w net.core.netdev_max_backlog=10000

# 2. 실행
sudo ./dumb-tpacket --pcap-buffer 8388608 eth0 wlan0

# 3. 현실적인 대역폭
iperf3 -c <server> -u -b 150M -t 60 -R

# 결과:
# - 에러 0개 또는 매우 적음 (<100)
# - Dropped < 1%
# - 실제 처리량: 140-150 Mbps (안정적)
```

---

## 자동 설정 스크립트

```bash
#!/bin/bash
# optimize-for-udp.sh

ETH_IF=eth0
WLAN_IF=wlan0

echo "=== UDP 고속 전송을 위한 시스템 최적화 ==="

# 1. TX 큐 증가
echo "1. TX 큐 크기 증가..."
sudo ip link set $ETH_IF txqueuelen 10000
sudo ip link set $WLAN_IF txqueuelen 10000
echo "  $ETH_IF: $(ip link show $ETH_IF | grep -o 'qlen [0-9]*')"
echo "  $WLAN_IF: $(ip link show $WLAN_IF | grep -o 'qlen [0-9]*')"

# 2. 커널 버퍼 증가
echo ""
echo "2. 커널 네트워크 버퍼 증가..."
sudo sysctl -w net.core.wmem_max=16777216 > /dev/null
sudo sysctl -w net.core.wmem_default=16777216 > /dev/null
sudo sysctl -w net.core.rmem_max=16777216 > /dev/null
sudo sysctl -w net.core.rmem_default=16777216 > /dev/null
sudo sysctl -w net.core.netdev_max_backlog=10000 > /dev/null
echo "  wmem_max: 16MB"
echo "  rmem_max: 16MB"
echo "  netdev_max_backlog: 10000"

# 3. 무선 파워 세이빙 off
echo ""
echo "3. 무선 파워 세이빙 비활성화..."
sudo iwconfig $WLAN_IF power off 2>/dev/null && echo "  $WLAN_IF: power off" || echo "  $WLAN_IF: 파워 관리 설정 실패"

# 4. Ring buffer 증가
echo ""
echo "4. Ring buffer 크기 증가..."
sudo ethtool -G $ETH_IF rx 4096 tx 4096 2>/dev/null && echo "  $ETH_IF: rx/tx 4096" || echo "  $ETH_IF: ring buffer 조정 실패"
sudo ethtool -G $WLAN_IF rx 4096 tx 4096 2>/dev/null && echo "  $WLAN_IF: rx/tx 4096" || echo "  $WLAN_IF: ring buffer 조정 실패"

echo ""
echo "=== 최적화 완료! ==="
echo ""
echo "브릿지 시작:"
echo "  sudo ./dumb-tpacket eth0 wlan0"
echo ""
echo "권장 테스트 명령:"
echo "  iperf3 -c <server> -u -b 150M -t 60 -R"
```

---

## 문제 지속 시 진단

### 1. 실제 무선 처리량 측정

```bash
# 브릿지 없이 직접 무선 성능 측정
# 무선 디바이스에서 직접:
iperf3 -s

# 유선 PC에서:
iperf3 -c <무선IP> -u -b 300M -t 30

# 이 값이 실제 무선 한계
# 예: 250 Mbps까지만 된다면
# → 브릿지로 250 Mbps 이상은 불가능
```

### 2. 병목 지점 찾기

```bash
# 실시간 모니터링
watch -n 0.5 "ip -s link show eth0 | grep -A 2 'TX:' ; echo ; ip -s link show wlan0 | grep -A 2 'TX:'"

# iperf3 실행하면서 관찰
# dropped 또는 errors 증가하는 인터페이스 = 병목
```

### 3. 커널 로그 확인

```bash
# 버퍼 오버플로우 관련 메시지
dmesg | tail -100 | grep -E "buffer|queue|drop"
```

---

## 요약

### 즉시 조치 (순서대로)

```bash
# 1. TX 큐 증가
sudo ip link set eth0 txqueuelen 10000
sudo ip link set wlan0 txqueuelen 10000

# 2. 브릿지 시작 (TPACKET 버전 권장)
sudo ./dumb-tpacket eth0 wlan0

# 3. 현실적인 대역폭으로 테스트
iperf3 -c <server> -u -b 100M -t 30 -R
# (UDP는 -P 옵션 사용 안 함!)

# 4. 통계 확인
sudo kill -USR1 $(pidof dumb-tpacket)
```

### 기대 결과

- **에러 없음** 또는 **매우 적음** (<100 에러 / 30초)
- **Dropped < 1%**
- **안정적인 처리량: 100-200 Mbps** (무선 환경에 따라)

### 500 Mbps 달성 불가능한 이유

1. **무선 실제 처리량**: 300-400 Mbps (NXP 88W9098 현실적 한계)
2. **브릿지 오버헤드**: ~10-20% 추가 손실
3. **UDP 흐름 제어 없음**: 버퍼 관리 어려움
4. **실제 목표**: 150-250 Mbps (안정적)

### 500 Mbps 목표라면

- **TCP 사용** (흐름 제어 있음)
- **유선 → 무선 방향** (무선이 TX 쪽)
- **무선 환경 최적화** (간섭 없음, 근거리, 좋은 안테나)
- **802.11ac/ax 사용** (802.11n은 불가능)
