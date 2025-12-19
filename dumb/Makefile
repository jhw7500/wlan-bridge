CC = gcc
# 성능 최적화 플래그
# -O3: 최대 최적화
# -march=native: 현재 CPU 아키텍처에 맞게 최적화
# -flto: Link-Time Optimization
# -fomit-frame-pointer: 프레임 포인터 생략으로 레지스터 여유 확보
CFLAGS = -Wall -Wextra -O3 -march=native -pthread -flto -fomit-frame-pointer
LDFLAGS_PCAP = -lpcap -pthread -flto
LDFLAGS_TPACKET = -pthread -flto

TARGET_PCAP = dumb
TARGET_TPACKET = dumb-tpacket

SRC_PCAP = dumb.c
SRC_TPACKET = dumb-tpacket.c

all: $(TARGET_PCAP) $(TARGET_TPACKET)

# libpcap version (Phase 1 + 2.3)
$(TARGET_PCAP): $(SRC_PCAP)
	$(CC) $(CFLAGS) -o $(TARGET_PCAP) $(SRC_PCAP) $(LDFLAGS_PCAP)

# TPACKET_V3 version (Phase 2.1)
$(TARGET_TPACKET): $(SRC_TPACKET)
	$(CC) $(CFLAGS) -o $(TARGET_TPACKET) $(SRC_TPACKET) $(LDFLAGS_TPACKET)

clean:
	rm -f $(TARGET_PCAP) $(TARGET_TPACKET)

.PHONY: all clean
