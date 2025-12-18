CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LDFLAGS_PCAP = -lpcap -pthread
LDFLAGS_TPACKET = -pthread

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
