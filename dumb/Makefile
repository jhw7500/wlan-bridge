CC = gcc
# 성능 최적화 플래그
# -O3: 최대 최적화
# -march=native: 현재 CPU 아키텍처에 맞게 최적화
# -flto: Link-Time Optimization
# -fomit-frame-pointer: 프레임 포인터 생략으로 레지스터 여유 확보
CFLAGS = -Wall -Wextra -O3 -march=native -pthread -flto -fomit-frame-pointer
LDFLAGS_PCAP = -lpcap -pthread -flto
LDFLAGS_TPACKET = -pthread -flto

# Output directory (can be overridden: make BINDIR=.)
BINDIR ?= bin

TARGET_PCAP = dumb
TARGET_TPACKET = dumb-tpacket

SRC_PCAP = dumb.c
SRC_TPACKET = dumb-tpacket.c

all: $(BINDIR) $(BINDIR)/$(TARGET_PCAP) $(BINDIR)/$(TARGET_TPACKET)

# Create output directory if it doesn't exist
$(BINDIR):
	mkdir -p $(BINDIR)

# libpcap version (Phase 1 + 2.3)
$(BINDIR)/$(TARGET_PCAP): $(SRC_PCAP)
	$(CC) $(CFLAGS) -o $(BINDIR)/$(TARGET_PCAP) $(SRC_PCAP) $(LDFLAGS_PCAP)

# TPACKET_V3 version (Phase 2.1)
$(BINDIR)/$(TARGET_TPACKET): $(SRC_TPACKET)
	$(CC) $(CFLAGS) -o $(BINDIR)/$(TARGET_TPACKET) $(SRC_TPACKET) $(LDFLAGS_TPACKET)

clean:
	rm -f $(BINDIR)/$(TARGET_PCAP) $(BINDIR)/$(TARGET_TPACKET)
	@if [ "$(BINDIR)" != "." ]; then rmdir --ignore-fail-on-non-empty $(BINDIR) 2>/dev/null || true; fi

.PHONY: all clean
