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
