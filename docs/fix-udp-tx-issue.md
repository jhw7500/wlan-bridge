# UDP TX 측정 안 되는 문제 해결 가이드

## 문제 상황

- **TCP**: 양방향 모두 RX/TX 정상 측정 ✅
- **UDP**: RX는 되는데 TX가 안 됨 ❌

## 원인 분석

### TCP vs UDP 트래픽 패턴 차이

```
TCP (양방향 트래픽 자동 발생):
Client ──SYN──────────────► Server
Client ◄────────SYN+ACK──── Server
Client ──ACK──────────────► Server
Client ──DATA─────────────► Server
Client ◄────────ACK──────── Server  ← 양방향 트래픽!
Client ──DATA─────────────► Server
Client ◄────────ACK──────── Server

브릿지 관점:
- eth0 RX: SYN, ACK, DATA (클라이언트 → 서버)
- wlan0 TX: SYN, ACK, DATA (서버로 전달)
- wlan0 RX: SYN-ACK, ACK (서버 → 클라이언트)
- eth0 TX: SYN-ACK, ACK (클라이언트로 전달)
→ 양쪽 모두 RX/TX 발생! ✅


UDP (순수 단방향):
Client ──DATA─────────────► Server
Client ──DATA─────────────► Server
Client ──DATA─────────────► Server
(ACK 없음!)

iperf3 UDP 모드:
Client ──UDP DATA──────────► Server (99.9%)
Client ◄──통계 패킷(작음)─── Server (0.1%)

브릿지 관점:
- eth0 RX: UDP DATA (대량)
- wlan0 TX: UDP DATA (대량)
- wlan0 RX: 통계 패킷 (거의 없음) ← TX 방향은 거의 트래픽 없음!
- eth0 TX: 통계 패킷 (거의 없음)
→ RX는 많지만 TX는 거의 없어 보임! ❌
```

## 해결 방법

### 1. UDP 양방향 트래픽 강제 생성

#### A. 두 개의 iperf3 인스턴스 동시 실행

```bash
# 터미널 1: 서버 (유선 쪽)
iperf3 -s -p 5201

# 터미널 2: 서버 (무선 쪽)
iperf3 -s -p 5202

# 터미널 3: 클라이언트 1 (유선 → 무선)
iperf3 -c <무선IP> -u -b 500M -t 60 -p 5202 &

# 터미널 4: 클라이언트 2 (무선 → 유선, 동시 실행)
iperf3 -c <유선IP> -u -b 500M -t 60 -p 5201 &

# 브릿지 통계 확인
watch -n 1 "sudo kill -USR1 \$(pidof dumb)"

# 이제 양방향 모두 RX/TX 증가해야 함!
```

#### B. iperf3 --bidir 옵션 (TCP만 지원, UDP는 미지원)

```bash
# 이 방법은 UDP에서 작동 안 함!
iperf3 -c <server> -u -b 500M --bidir  # ❌ UDP는 미지원
```

### 2. tcpdump로 실제 패킷 흐름 확인

```bash
# 터미널 1: eth0 패킷 캡처
sudo tcpdump -i eth0 -n udp -c 1000 -q

# 터미널 2: wlan0 패킷 캡처
sudo tcpdump -i wlan0 -n udp -c 1000 -q

# 터미널 3: iperf3 UDP 테스트
iperf3 -c <server> -u -b 500M -t 30 -R

# 출력 분석:
# eth0: 대량의 UDP 패킷 (서버 → 클라이언트 방향)
# wlan0: 거의 없거나 매우 적은 패킷 (제어 채널만)
```

### 3. 브릿지 통계 상세 분석

```bash
# 1초마다 통계 확인하면서 iperf3 실행
watch -n 1 "sudo kill -USR1 \$(pidof dumb)"

# iperf3 UDP 업로드 (eth0 → wlan0)
iperf3 -c <server> -u -b 500M -t 30

# 예상 결과:
# Interface 0 (eth0):
#   RX: 15000000 packets  ← 대량 증가
#   TX: 5000 packets      ← 제어 패킷만 (매우 적음)
# Interface 1 (wlan0):
#   RX: 5000 packets      ← 제어 패킷만
#   TX: 15000000 packets  ← 대량 증가


# iperf3 UDP 다운로드 (wlan0 → eth0)
iperf3 -c <server> -u -b 500M -t 30 -R

# 예상 결과:
# Interface 0 (eth0):
#   RX: 5000 packets      ← 제어 패킷만 (적음)
#   TX: 15000000 packets  ← 대량 증가 (이게 안 보이면 문제!)
# Interface 1 (wlan0):
#   RX: 15000000 packets  ← 대량 증가
#   TX: 5000 packets      ← 제어 패킷만
```

### 4. 실제 문제인지 확인 (진단)

#### A. 패킷 카운트 확인

```bash
# iperf3 UDP 다운로드 시작 전
sudo kill -USR1 $(pidof dumb)
# eth0 TX: 12345 기록

# iperf3 실행
iperf3 -c <server> -u -b 500M -t 30 -R

# 30초 후 다시 확인
sudo kill -USR1 $(pidof dumb)
# eth0 TX: 12346 (거의 안 증가) ← 문제!
# eth0 TX: 15012345 (대량 증가) ← 정상!
```

#### B. 커널 통계 확인

```bash
# iperf3 시작 전
ip -s link show eth0 | grep "TX:"

# iperf3 실행
iperf3 -c <server> -u -b 500M -t 30 -R

# iperf3 종료 후
ip -s link show eth0 | grep "TX:"

# 커널 레벨에서도 TX 패킷이 증가했는지 확인
# 커널에서는 증가하는데 브릿지 통계에서 안 보인다면 → 브릿지 버그
# 커널에서도 안 증가한다면 → 라우팅/방화벽 문제
```

## 5. 가능한 원인별 해결

### 원인 1: UDP 제어 채널이 다른 경로로 가는 경우

```bash
# iperf3는 TCP 제어 채널 + UDP 데이터 채널 사용
# TCP: 5201 (기본)
# UDP: 5201 (데이터), 랜덤 포트 (제어)

# 해결: 명시적 포트 지정
iperf3 -c <server> -u -b 500M -t 30 -R --cport 12345
```

### 원인 2: 방화벽/필터링

```bash
# 방화벽 규칙 확인
sudo iptables -L -n -v

# 임시로 방화벽 비활성화 테스트
sudo iptables -P INPUT ACCEPT
sudo iptables -P OUTPUT ACCEPT
sudo iptables -P FORWARD ACCEPT
sudo iptables -F

# 다시 iperf3 테스트
iperf3 -c <server> -u -b 500M -t 30 -R
```

### 원인 3: 브릿지가 UDP 역방향을 못 잡는 경우

**코드 검증**: `dumb.c`의 `ph()` 함수에서 UDP 패킷도 제대로 처리하는지 확인

```c
// ph() 함수에 임시 디버깅 추가
static void ph(unsigned char *ifp, const struct pcap_pkthdr *hdr, const unsigned char *data)
{
    unsigned int i = (unsigned int)((uintptr_t)ifp);
    unsigned int peer = i ^ 1;

    if (!(hdr && data)) return;

    // UDP 패킷 감지 디버깅
    if (hdr->caplen > 20) {
        const uint8_t *ip_hdr = data + 14;  // Ethernet 헤더 건너뜀
        if (ip_hdr[9] == 17) {  // Protocol = UDP
            static atomic_ulong udp_count[2] = {0};
            unsigned long cnt = atomic_fetch_add(&udp_count[i], 1);
            if (cnt % 10000 == 0) {
                fprintf(stderr, "UDP packet on if%u → if%u, size=%u (count: %lu)\n",
                        i, peer, hdr->caplen, cnt + 1);
            }
        }
    }

    atomic_fetch_add(&stats.rx_packets[i], 1);
    // ... 나머지 코드
}
```

### 원인 4: 무선 드라이버 UDP TX 큐 문제

```bash
# 무선 인터페이스 통계 확인
ethtool -S wlan0 | grep -i udp
ethtool -S wlan0 | grep -i queue

# TX 큐 크기 확인 및 조정
ip link show wlan0 | grep qlen

# TX 큐 크기 증가
sudo ip link set wlan0 txqueuelen 10000
```

## 6. 최종 검증 스크립트

```bash
#!/bin/bash
# udp-bidirectional-test.sh

SERVER_IP="192.168.1.100"

echo "=== UDP 양방향 브릿지 테스트 ==="
echo ""

# 초기 통계
echo "초기 통계:"
sudo kill -USR1 $(pidof dumb)
sleep 2

echo ""
echo "1. UDP 업로드 (eth0 RX, wlan0 TX)"
iperf3 -c $SERVER_IP -u -b 500M -t 30
sleep 2

echo ""
echo "중간 통계:"
sudo kill -USR1 $(pidof dumb)
sleep 2

echo ""
echo "2. UDP 다운로드 (wlan0 RX, eth0 TX) ← 이게 중요!"
iperf3 -c $SERVER_IP -u -b 500M -t 30 -R
sleep 2

echo ""
echo "최종 통계:"
sudo kill -USR1 $(pidof dumb)

echo ""
echo "=== 커널 레벨 통계 비교 ==="
ip -s link show eth0 | grep -A 1 "RX:\|TX:"
ip -s link show wlan0 | grep -A 1 "RX:\|TX:"
```

## 7. 예상되는 정상 동작

### 정상 케이스 (TX도 측정됨)

```
=== UDP 다운로드 후 통계 ===
Interface 0 (eth0):
  RX:        50000 packets      ← 제어 패킷 (적음)
  TX:        8500000 packets    ← UDP 데이터 (대량) ✅

Interface 1 (wlan0):
  RX:        8500000 packets    ← UDP 데이터 (대량)
  TX:        50000 packets      ← 제어 패킷 (적음)
```

### 문제 케이스 (TX 측정 안 됨)

```
=== UDP 다운로드 후 통계 ===
Interface 0 (eth0):
  RX:        50000 packets      ← 제어 패킷만
  TX:        50100 packets      ← 거의 안 증가! ❌

Interface 1 (wlan0):
  RX:        8500000 packets    ← UDP 데이터 많음
  TX:        50000 packets      ← 제어 패킷만
```

**이 경우 실제 원인**:
1. 브릿지가 패킷을 못 잡고 있음 (pcap 필터 문제?)
2. 무선 → 유선 방향 라우팅 문제
3. 커널이 직접 라우팅해서 브릿지를 우회

## 8. 근본 원인 찾기

```bash
# 1. 커널이 직접 라우팅하는지 확인
cat /proc/sys/net/ipv4/ip_forward
# 1이면 → IP forwarding 활성화됨 (브릿지 우회 가능)
# 0이면 → OK

# 2. 브릿지가 promiscuous 모드인지 확인
ip link show eth0 | grep PROMISC
ip link show wlan0 | grep PROMISC
# PROMISC가 있어야 모든 패킷 캡처 가능

# 3. tcpdump로 실제 패킷 확인
sudo tcpdump -i eth0 -n -c 100 'udp and dst <client_ip>'
# UDP 다운로드 시 eth0에 패킷이 보이는가?
# 보인다 → 브릿지 문제
# 안 보인다 → 라우팅 문제
```

---

## 요약

### UDP TX 안 되는 이유 (가능성 순)

1. **정상 동작**: UDP는 단방향이라 제어 패킷만 역방향 → TX 매우 적음 ✅
2. **브릿지 문제**: 무선 → 유선 방향 패킷을 못 잡음 (pcap 필터?)
3. **라우팅 우회**: 커널이 직접 라우팅해서 브릿지 우회
4. **무선 드라이버**: TX 큐 또는 특정 UDP 문제

### 즉시 확인할 명령

```bash
# 1. 커널 통계와 브릿지 통계 비교
ip -s link show eth0  # 커널 레벨
sudo kill -USR1 $(pidof dumb)  # 브릿지 레벨

# 2. tcpdump로 패킷 흐름 확인
sudo tcpdump -i eth0 -n udp -c 100

# 3. UDP 다운로드 테스트
iperf3 -c <server> -u -b 500M -t 30 -R

# 4. 통계 다시 확인
sudo kill -USR1 $(pidof dumb)
```

### 정상 vs 비정상 판단

- **커널 TX 증가 + 브릿지 TX 증가** → ✅ 정상
- **커널 TX 증가 + 브릿지 TX 안 증가** → ❌ 브릿지 버그
- **커널 TX 안 증가** → ❌ 라우팅/방화벽 문제
