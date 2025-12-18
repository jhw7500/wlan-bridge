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

