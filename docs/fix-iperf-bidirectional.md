# iperf3 양방향 측정 가이드 (브릿지 환경)

## 문제: RX는 측정되는데 TX가 측정 안 됨

### 원인 분석

브릿지 환경에서 **RX/TX 개념**은 **iperf3 서버/클라이언트 위치**에 따라 달라집니다.

```
시나리오 1: iperf3 서버가 무선(wlan0) 쪽에 있는 경우
┌─────────┐         ┌─────────┐         ┌─────────┐
│ Client  │  eth0   │ Bridge  │  wlan0  │ Server  │
│ (PC)    ├────────►│         ├────────►│ (AP)    │
└─────────┘         └─────────┘         └─────────┘
                    RX on eth0 ✓
                    TX on wlan0 ✓

Client → Server: eth0 RX, wlan0 TX (정상 측정)
Server → Client: wlan0 RX, eth0 TX (여기가 문제!)


시나리오 2: iperf3 서버가 유선(eth0) 쪽에 있는 경우
┌─────────┐         ┌─────────┐         ┌─────────┐
│ Server  │  eth0   │ Bridge  │  wlan0  │ Client  │
│ (PC)    │◄────────┤         │◄────────┤ (AP)    │
└─────────┘         └─────────┘         └─────────┘
                    TX on eth0 ?
                    RX on wlan0 ✓
```

### 해결 방법

## 1. 양방향 트래픽 측정 (정확한 방법)

### A. iperf3 양방향 모드 사용

```bash
# 서버 (유선 쪽 PC)
iperf3 -s

# 클라이언트 (무선 쪽 디바이스) - 양방향 동시 측정
iperf3 -c <server_ip> -d --bidir -t 60

# 또는 더 정확한 방법: 별도 포트로 양방향
iperf3 -c <server_ip> -t 60 -P 4                    # 업로드 (Client → Server)
iperf3 -c <server_ip> -t 60 -P 4 -R                 # 다운로드 (Server → Client)
```

### B. 두 개의 iperf3 서버 실행

```bash
# 터미널 1: 유선 쪽 서버
iperf3 -s -p 5201

# 터미널 2: 무선 쪽 서버 (다른 포트)
iperf3 -s -p 5202

# 클라이언트에서 양방향 테스트
iperf3 -c <유선IP> -p 5201 -t 60 -P 4     # 무선 → 유선 (wlan0 RX, eth0 TX)
iperf3 -c <무선IP> -p 5202 -t 60 -P 4     # 유선 → 무선 (eth0 RX, wlan0 TX)
```

### C. 동시 측정 스크립트

```bash
#!/bin/bash
# 파일명: test-bridge-throughput.sh

SERVER_IP="192.168.1.100"  # 서버 IP
DURATION=60

echo "=== 브릿지 양방향 처리량 테스트 ==="
echo ""

# 브릿지 통계 초기화
sudo kill -USR1 $(pidof dumb) 2>/dev/null || true
sleep 2

echo "1. 업로드 테스트 (Client → Server)"
iperf3 -c $SERVER_IP -t $DURATION -P 4 | tee upload.log

sleep 5

echo ""
echo "2. 다운로드 테스트 (Server → Client, -R 옵션)"
iperf3 -c $SERVER_IP -t $DURATION -P 4 -R | tee download.log

sleep 2

echo ""
echo "3. 브릿지 통계 확인"
sudo kill -USR1 $(pidof dumb)

echo ""
echo "=== 결과 요약 ==="
echo "업로드:"
grep "sender" upload.log | tail -1
echo "다운로드:"
grep "receiver" download.log | tail -1
```

## 2. -P 4 옵션 문제 해결

### 문제: 병렬 스트림이 TX 측정에 영향

`-P 4`는 4개의 TCP 스트림을 동시에 생성하는데, 브릿지 환경에서 문제가 될 수 있습니다:

1. **포트 범위**: 4개 스트림 = 4개 다른 포트
2. **커널 라우팅**: 브릿지가 모든 포트를 올바르게 포워딩하는지 확인 필요
3. **통계 집계**: 브릿지 통계가 모든 스트림을 합산하는지 확인

### 해결책

#### A. 단일 스트림으로 테스트

```bash
# -P 옵션 제거하고 단순 테스트
iperf3 -c <server> -t 60

# RX/TX 모두 측정되는지 확인
```

#### B. UDP 모드로 테스트

TCP는 흐름 제어로 인해 복잡할 수 있으므로 UDP로 먼저 확인:

```bash
# UDP 업로드 (500 Mbps 목표)
iperf3 -c <server> -u -b 500M -t 60

# UDP 다운로드
iperf3 -c <server> -u -b 500M -t 60 -R

# 패킷 손실률 확인 (0.5% 미만이 목표)
```

#### C. 브릿지 통계 실시간 모니터링

```bash
# 터미널 1: 브릿지 실행
sudo ./dumb eth0 wlan0

# 터미널 2: 통계 모니터링
watch -n 1 "sudo kill -USR1 \$(pidof dumb)"

# 터미널 3: iperf3 실행
iperf3 -c <server> -t 60 -P 4 -R

# 터미널 2에서 RX/TX 카운터 실시간 확인
```

## 3. 디버깅: 왜 TX가 측정 안 되나?

### 체크리스트

```bash
# 1. 브릿지가 실행 중인지 확인
ps aux | grep dumb

# 2. 양쪽 인터페이스가 UP 상태인지
ip link show eth0
ip link show wlan0

# 3. IP 라우팅 확인
ip route show

# 4. 방화벽 확인
sudo iptables -L -n -v

# 5. 브릿지 통계 확인 (TX 카운터가 0인지?)
sudo kill -USR1 $(pidof dumb)

# 6. tcpdump로 실제 패킷 흐름 확인
sudo tcpdump -i eth0 -c 100 -n
sudo tcpdump -i wlan0 -c 100 -n
```

## 4. 정확한 측정 방법

### 시나리오: eth0 ↔ wlan0 브릿지

```bash
# 설정
PC (유선) ─── eth0 ─── [브릿지] ─── wlan0 ─── AP (무선)

# A. PC → AP 방향 (eth0 RX, wlan0 TX)
# 서버: AP 쪽 디바이스
iperf3 -s

# 클라이언트: PC
iperf3 -c <AP_IP> -t 60 -P 4

# 브릿지 통계 확인
# RX on eth0: 증가 ✓
# TX on wlan0: 증가 ✓


# B. AP → PC 방향 (wlan0 RX, eth0 TX)
# 서버: PC
iperf3 -s

# 클라이언트: AP 쪽 디바이스
iperf3 -c <PC_IP> -t 60 -P 4 -R  # -R 옵션 중요!

# 브릿지 통계 확인
# RX on wlan0: 증가 ✓
# TX on eth0: 증가해야 함 ✓
```

## 5. 문제 지속 시 추가 조치

### A. 브릿지 로그 레벨 증가

dumb.c를 수정하여 더 상세한 로그 추가:

```c
// ph() 함수에서
atomic_fetch_add(&stats.rx_packets[i], 1);

// 디버깅용 로그 (임시)
static atomic_ulong log_counter = 0;
if (atomic_fetch_add(&log_counter, 1) % 10000 == 0) {
    fprintf(stderr, "DEBUG: RX on if%u, forwarding to if%u, size=%u\n",
            i, peer, hdr->caplen);
}
```

### B. 커널 네트워크 통계 확인

```bash
# 인터페이스별 상세 통계
ip -s link show eth0
ip -s link show wlan0

# RX/TX 패킷 카운터 확인
# errors, dropped 항목 체크
```

### C. 무선 인터페이스 특이사항

```bash
# 무선 드라이버 통계
ethtool -S wlan0 | grep -i drop
ethtool -S wlan0 | grep -i error

# 무선 연결 상태
iw dev wlan0 link
iw dev wlan0 station dump
```

---

## 요약

### TX 측정 안 되는 이유 (가능성)

1. **iperf3 방향 문제**: `-R` 옵션 필요
2. **서버/클라이언트 위치**: 반대 방향 테스트 필요
3. **통계 카운터 버그**: 브릿지 코드 검증 필요
4. **무선 드라이버 이슈**: wlan0 TX 큐 문제

### 즉시 시도할 명령

```bash
# 1. 단순 양방향 테스트
iperf3 -c <server> -t 30              # 업로드
iperf3 -c <server> -t 30 -R           # 다운로드 (TX 방향)

# 2. 브릿지 통계 확인
sudo kill -USR1 $(pidof dumb)

# 3. 두 방향 모두 TX 카운터 증가하는지 확인!
```

### 기대 결과

```
=== Packet Statistics (uptime: 60 seconds) ===
  Interface 0 (eth0):
    RX:      1234567 packets (20576 pps)  ← 업로드 시 증가
    TX:      1234000 packets (20567 pps)  ← 다운로드 시 증가 (이게 안 된다면 문제!)

  Interface 1 (wlan0):
    RX:      1234000 packets (20567 pps)  ← 다운로드 시 증가
    TX:      1234567 packets (20576 pps)  ← 업로드 시 증가
```
