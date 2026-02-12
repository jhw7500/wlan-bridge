CC ?= gcc
CFLAGS ?= -Wall -Wextra -O3 -pthread -fomit-frame-pointer -I.
LDFLAGS ?= -lpcap -pthread
LDFLAGS_TPACKET ?= -pthread

# Output directories
RELEASEDIR ?= release
DEBUGDIR ?= debug
TESTDIR ?= tests/bin

STRIP ?= strip

TARGET = wbridge
TARGET_TPACKET = wbridge-tpacket
TEST_TARGET = test_filter

# Source files
SRCS = main.c bridge.c config.c stats.c packet.c filter.c bridge_packet_handler.c
OBJS_RELEASE = $(SRCS:%.c=$(RELEASEDIR)/%.o)
OBJS_DEBUG = $(SRCS:%.c=$(DEBUGDIR)/%.o)

SRC_TPACKET = wbridge-tpacket.c

TEST_SRCS = tests/test_filter.c filter.c packet.c
TEST_OBJS = $(TEST_SRCS:%.c=$(TESTDIR)/%.o)

all: release debug tests

$(RELEASEDIR)/.dir $(DEBUGDIR)/.dir $(TESTDIR)/.dir $(TESTDIR)/unit/.dir:
	mkdir -p $(RELEASEDIR) $(DEBUGDIR) $(TESTDIR) $(TESTDIR)/unit
	@touch $@

# Variants
RELEASE_CFLAGS ?= $(CFLAGS) -flto
DEBUG_CFLAGS ?= $(CFLAGS) -Og -g3

release: $(RELEASEDIR)/.dir $(RELEASEDIR)/$(TARGET) $(RELEASEDIR)/$(TARGET_TPACKET)
debug: $(DEBUGDIR)/.dir $(DEBUGDIR)/$(TARGET) $(DEBUGDIR)/$(TARGET_TPACKET)
tests: $(TESTDIR)/.dir $(TESTDIR)/$(TEST_TARGET)

# Release build (wbridge)
$(RELEASEDIR)/$(TARGET): $(OBJS_RELEASE)
	$(CC) $(RELEASE_CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

# Release build (wbridge-tpacket)
$(RELEASEDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -o $@ $< $(LDFLAGS_TPACKET)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

$(RELEASEDIR)/%.o: %.c | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -c $< -o $@

# Debug build
$(DEBUGDIR)/$(TARGET): $(OBJS_DEBUG)
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS)

$(DEBUGDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $< $(LDFLAGS_TPACKET)

$(DEBUGDIR)/%.o: %.c | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

# Unit Tests
$(TESTDIR)/$(TEST_TARGET): tests/test_filter.c filter.c packet.c | $(TESTDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS)

run-tests: tests
	./$(TESTDIR)/$(TEST_TARGET)

clean:
	rm -rf $(RELEASEDIR) $(DEBUGDIR) $(TESTDIR)

.PHONY: all release debug tests run-tests clean
