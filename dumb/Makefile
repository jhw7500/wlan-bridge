CC ?= gcc
# 성능 최적화 플래그
# -O3: 최대 최적화
# -march=native: 현재 CPU 아키텍처에 맞게 최적화
# -flto: Link-Time Optimization
# -fomit-frame-pointer: 프레임 포인터 생략으로 레지스터 여유 확보
#CFLAGS = -Wall -Wextra -O3 -march=native -pthread -flto -fomit-frame-pointer
CFLAGS ?= -Wall -Wextra -O3 -pthread -fomit-frame-pointer

# Default link flags.
# - Keep LTO off by default for portability (especially cross-compiles).
LDFLAGS_PCAP ?= -lpcap -pthread
LDFLAGS_TPACKET ?= -pthread

# Output directories
# - release/: default build output (stripped)
# - debug/: debug build output (unstripped)
RELEASEDIR ?= release
DEBUGDIR ?= debug

STRIP ?= strip

TARGET_PCAP = dumb
TARGET_TPACKET = dumb-tpacket

SRC_PCAP = dumb.c
SRC_TPACKET = dumb-tpacket.c

all: release debug

$(RELEASEDIR)/.dir:
	mkdir -p $(RELEASEDIR)
	@touch $@

$(DEBUGDIR)/.dir:
	mkdir -p $(DEBUGDIR)
	@touch $@

# Build variants
RELEASE_CFLAGS ?= $(CFLAGS)
DEBUG_CFLAGS ?= $(CFLAGS) -Og -g3

release: $(RELEASEDIR)/.dir $(RELEASEDIR)/$(TARGET_PCAP) $(RELEASEDIR)/$(TARGET_TPACKET)

debug: $(DEBUGDIR)/.dir $(DEBUGDIR)/$(TARGET_PCAP) $(DEBUGDIR)/$(TARGET_TPACKET)

# libpcap version (Phase 1 + 2.3)
$(RELEASEDIR)/$(TARGET_PCAP): $(SRC_PCAP) | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -o $@ $(SRC_PCAP) $(LDFLAGS_PCAP)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

$(DEBUGDIR)/$(TARGET_PCAP): $(SRC_PCAP) | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $(SRC_PCAP) $(LDFLAGS_PCAP)

# TPACKET_V3 version (Phase 2.1)
$(RELEASEDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -o $@ $(SRC_TPACKET) $(LDFLAGS_TPACKET)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

$(DEBUGDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $(SRC_TPACKET) $(LDFLAGS_TPACKET)

clean:
	rm -f $(RELEASEDIR)/$(TARGET_PCAP) $(RELEASEDIR)/$(TARGET_TPACKET)
	rm -f $(DEBUGDIR)/$(TARGET_PCAP) $(DEBUGDIR)/$(TARGET_TPACKET)
	@rm -f $(RELEASEDIR)/.dir $(DEBUGDIR)/.dir
	@rmdir --ignore-fail-on-non-empty $(RELEASEDIR) 2>/dev/null || true
	@rmdir --ignore-fail-on-non-empty $(DEBUGDIR) 2>/dev/null || true

.PHONY: all release debug clean
