# MTU 문제 해결 가이드

## 문제: pcap_inject() "Message too long" 에러

### 1. MTU 확인

```bash
# 인터페이스 MTU 확인
ip link show eth0
ip link show wlan0

# 예상 출력:
# eth0: mtu 1500
# wlan0: mtu 1500 (또는 더 작을 수 있음)
```

### 2. 임시 해결: MTU 조정

```bash
# 무선 인터페이스 MTU를 줄임 (안전한 값)
sudo ip link set wlan0 mtu 1400

# 또는 양쪽 모두 조정
sudo ip link set eth0 mtu 1400
sudo ip link set wlan0 mtu 1400
```

### 3. 영구 해결: 코드 수정

#### 방법 A: 패킷 크기 검증 및 드롭

`dumb.c` 파일에서 `ph()` 함수 수정:

```c
static void ph(unsigned char *ifp, const struct pcap_pkthdr *hdr, const unsigned char *data)
{
    unsigned int i = (unsigned int)((uintptr_t)ifp);
    unsigned int peer = i ^ 1;

    if (!(hdr && data)) return;

    atomic_fetch_add(&stats.rx_packets[i], 1);

    if (!both_ready() || ifs.tx[peer] == NULL) {
        atomic_fetch_add(&stats.dropped[i], 1);
        return;
    }

    // **추가**: MTU 체크 (1500바이트 초과 시 드롭)
    #define MAX_PACKET_SIZE 1500
    if (hdr->caplen > MAX_PACKET_SIZE) {
        atomic_fetch_add(&stats.dropped[i], 1);

        static atomic_ulong mtu_drop_count = 0;
        unsigned long mtu_cnt = atomic_fetch_add(&mtu_drop_count, 1);
        if (mtu_cnt % 100 == 0) {
            fprintf(stderr, "WARNING: Dropped oversized packet %u bytes (count: %lu)\n",
                    hdr->caplen, mtu_cnt + 1);
        }
        return;
    }

    int ret = pcap_inject(ifs.tx[peer], data, hdr->caplen);
    // ... 기존 코드
}
```

#### 방법 B: Path MTU Discovery 활성화

```bash
# 커널 PMTUD 활성화
sudo sysctl -w net.ipv4.ip_no_pmtu_disc=0

# 영구 설정
echo "net.ipv4.ip_no_pmtu_disc=0" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

### 4. 디버깅: 패킷 크기 분포 확인

임시로 패킷 크기 로그 추가:

```c
// ph() 함수 내부
static atomic_ulong size_hist[4] = {0};  // 0-500, 501-1000, 1001-1500, 1500+

if (hdr->caplen <= 500) atomic_fetch_add(&size_hist[0], 1);
else if (hdr->caplen <= 1000) atomic_fetch_add(&size_hist[1], 1);
else if (hdr->caplen <= 1500) atomic_fetch_add(&size_hist[2], 1);
else atomic_fetch_add(&size_hist[3], 1);

// print_stats_impl()에서 출력
fprintf(stderr, "  Packet size distribution:\n");
fprintf(stderr, "    0-500:    %lu\n", atomic_load(&size_hist[0]));
fprintf(stderr, "    501-1000: %lu\n", atomic_load(&size_hist[1]));
fprintf(stderr, "    1001-1500: %lu\n", atomic_load(&size_hist[2]));
fprintf(stderr, "    >1500:    %lu (OVERSIZED)\n", atomic_load(&size_hist[3]));
```

### 5. NXP 88W9098 특화 설정

무선 칩이 특정 MTU 제한이 있을 수 있음:

```bash
# 드라이버 로그 확인
dmesg | grep -i mlan
dmesg | grep -i mtu

# 무선 인터페이스 정보 확인
iwconfig wlan0
iw dev wlan0 info
```

### 6. 권장 설정 (프로덕션)

```bash
# 안전한 MTU: 1400 (오버헤드 여유)
sudo ip link set eth0 mtu 1400
sudo ip link set wlan0 mtu 1400

# 브릿지 실행
sudo ./dumb eth0 wlan0

# 또는 TPACKET_V3 버전
sudo ./dumb-tpacket eth0 wlan0
```

### 7. 테스트

```bash
# MTU 설정 후 큰 패킷 테스트
ping -M do -s 1472 <target>  # 1500 - 20(IP) - 8(ICMP) = 1472
ping -M do -s 1372 <target>  # 1400 - 20 - 8 = 1372 (안전)

# iperf3로 처리량 재측정
iperf3 -c <server> -t 60
```

---

## 요약

**즉시 조치**:
```bash
sudo ip link set wlan0 mtu 1400
```

**장기 해결**:
- 코드에 MTU 체크 로직 추가
- 패킷 크기 분포 모니터링
- 필요시 fragmentation 지원 고려
