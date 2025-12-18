# wlan-bridge dumb

유/무선 L2 브릿지를 위한 간단한 userspace 브리지 도구입니다.

## 파일 구성

- `dumb.c`: 현재 사용(권장) 버전 (libpcap 기반)
- `dumb-tpacket.c`: 성능 실험용 (AF_PACKET + TPACKET_V3, libpcap 미사용)
- `Makefile`: 빌드
- `wifi_bridge@.service`: systemd 템플릿 유닛 예시
- `docs/`: 설계/변경 기록
- `archive/`: 단계별 스냅샷/원본/패치 보관

## 빌드

```bash
sudo apt-get install -y libpcap-dev
make
```

## 실행

```bash
sudo ./dumb eth0 wlan0
```

## 옵션(튜닝)

`dumb`는 기본 설정으로 동작하지만, 환경/드라이버/트래픽 특성에 따라 지터/CPU/처리량을 조정할 수 있도록 옵션을 제공합니다.

### 사용법

```bash
./dumb --help
./dumb [options] <if0> <if1>
```

### 언제/왜 쓰나(기대 효과)

- `--dispatch-budget N` (기본 64, env `DUMB_DISPATCH_BUDGET`)
  - 사용해야 되는 경우: 버스트 트래픽에서 ping 지터가 커지거나, 한 방향이 잠깐씩 밀리는 느낌이 있을 때
  - 기대 효과: `pcap_dispatch()`가 한 번에 처리하는 패킷 수를 제한해 지터를 줄이고 반응성을 높임
  - 주의: 너무 작게 잡으면 호출 빈도가 늘어 CPU 사용량이 늘 수 있음 (예: 32/64/128로 탐색 권장)

- `--no-rt` / `--rt-priority N` (env `DUMB_RT`, `DUMB_RT_PRIORITY`)
  - 사용해야 되는 경우: `SCHED_FIFO`가 시스템을 버벅이게 하거나 다른 작업을 굶기는 것 같을 때(또는 권한 부여가 어려울 때)
  - 기대 효과: RT 스케줄링을 끄거나 우선순위를 낮춰 시스템 안정성/동작 예측 가능성 개선

- `--no-affinity` (env `DUMB_AFFINITY`)
  - 사용해야 되는 경우: IRQ/softirq가 같은 코어에 몰려 경합이 심해지는 경우
  - 기대 효과: 코어 고정을 해제해 커널 네트워크 처리와의 경합을 줄일 수 있음(환경별 상이)

- `--no-mlock` (env `DUMB_MLOCK`)
  - 사용해야 되는 경우: systemd LimitMEMLOCK/cap 설정이 어렵거나 메모리 잠금이 오히려 문제를 만드는 경우
  - 기대 효과: `mlockall()`을 끄고도 동작 가능(레이턴시 지터가 약간 늘 수 있음)

- `--snaplen N` / `--pcap-buffer BYTES` / `--timeout-ms N` / `--no-immediate` / `--no-promisc`
  - 사용해야 되는 경우: 드롭이 많거나(버퍼 부족), CPU가 과도하게 쓰이거나, 특정 환경에서 immediate/promisc가 문제를 만들 때
  - 기대 효과:
    - `snaplen`을 줄이면 CPU/복사량 감소(단, 큰 프레임은 잘릴 수 있음)
    - `pcap-buffer`를 늘리면 커널/pcap 버퍼 오버플로 드롭 감소
    - `timeout-ms`/`immediate`는 레이턴시 vs CPU wakeup 트레이드오프
    - `promisc`를 끄면 브릿지가 필요한 프레임을 못 잡아 동작이 깨질 수 있으므로 특별한 이유가 없으면 유지 권장

### 예시

```bash
# 지터를 줄이려면 budget을 낮춰보기
sudo ./dumb --dispatch-budget 32 eth0 mlan0

# RT/affinity를 꺼서 시스템 버벅임을 피하기
sudo ./dumb --no-rt --no-affinity eth0 mlan0

# 환경변수로 제어(systemd에서 유용)
export DUMB_DISPATCH_BUDGET=64
export DUMB_RT=0
sudo ./dumb eth0 mlan0
```

통계 출력:

```bash
kill -USR1 $(pidof dumb)
```

## TPACKET 테스트(처리량 개선 실험)

`dumb-tpacket.c`는 libpcap 대신 AF_PACKET + TPACKET_V3 링 버퍼를 사용해 syscall/복사 오버헤드를 줄이는 실험용 구현입니다.

```bash
make dumb-tpacket
sudo ./dumb-tpacket eth0 wlan0
sudo kill -USR1 $(pidof dumb-tpacket)
```

## 참고: ping은 빨라졌는데 iperf가 비슷한 이유

- ping은 작은 패킷 위주의 RTT(레이턴시) 지표라서 스케줄링/페이지폴트/대기 방식 개선이 바로 효과가 납니다.
- iperf(TCP/UDP) 처리량은 무선 링크(MCS/재전송/aggregation), 드라이버 큐, 그리고 userspace 브릿지의 패킷당 오버헤드가 병목이 되는 경우가 많아 변화가 작을 수 있습니다.
- 처리량이 목표라면 `dumb-tpacket.c` 기반으로 튜닝/전환을 검토하세요.
