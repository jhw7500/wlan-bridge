CC ?= gcc
CFLAGS ?= -Wall -Wextra -O3 -pthread -fomit-frame-pointer -I.
LDFLAGS ?= -lpcap -pthread
LDFLAGS_TPACKET ?= -pthread

# Output directories
RELEASEDIR ?= release
DEBUGDIR ?= debug
TESTDIR ?= tests/bin

STRIP ?= strip

# BOARD_SUFFIX는 cross-build 시 보드별 바이너리 이름 분리에 사용 (예: _imx8, _imx93).
# 빈 값이면 host-native 빌드(기존 동작 유지).
BOARD_SUFFIX ?=

TARGET = wbridge$(BOARD_SUFFIX)
TARGET_TPACKET = wbridge-tpacket$(BOARD_SUFFIX)
TEST_TARGET = test_filter

# Source files
SRCS = main.c bridge.c config.c stats.c packet.c filter.c bridge_packet_handler.c
# .o 파일을 board별 서브디렉토리로 분리 (BOARD_SUFFIX가 빈 값이면 obj/ 그대로).
# 이렇게 하면 `make clean` 없이도 imx8/imx93을 같은 release/ 안에서 동시 빌드 가능.
OBJDIR_R = $(RELEASEDIR)/obj$(BOARD_SUFFIX)
OBJDIR_D = $(DEBUGDIR)/obj$(BOARD_SUFFIX)
OBJS_RELEASE = $(SRCS:%.c=$(OBJDIR_R)/%.o)
OBJS_DEBUG = $(SRCS:%.c=$(OBJDIR_D)/%.o)

SRC_TPACKET = wbridge-tpacket.c

TEST_SRCS = tests/test_filter.c filter.c packet.c
TEST_OBJS = $(TEST_SRCS:%.c=$(TESTDIR)/%.o)

all: release debug tests

# 일반화된 .dir 생성 룰 — 어떤 경로의 .dir이 필요해도 부모 디렉토리를 자동 생성.
%/.dir:
	@mkdir -p $(@D)
	@touch $@

# Variants
RELEASE_CFLAGS ?= $(CFLAGS) -flto
DEBUG_CFLAGS ?= $(CFLAGS) -Og -g3

release: $(RELEASEDIR)/$(TARGET) $(RELEASEDIR)/$(TARGET_TPACKET)
debug: $(DEBUGDIR)/$(TARGET) $(DEBUGDIR)/$(TARGET_TPACKET)
tests: $(TESTDIR)/$(TEST_TARGET)

# Release build (wbridge)
$(RELEASEDIR)/$(TARGET): $(OBJS_RELEASE) | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

# Release build (wbridge-tpacket)
$(RELEASEDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(RELEASEDIR)/.dir
	$(CC) $(RELEASE_CFLAGS) -o $@ $< $(LDFLAGS_TPACKET)
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true

$(OBJDIR_R)/%.o: %.c | $(OBJDIR_R)/.dir
	$(CC) $(RELEASE_CFLAGS) -c $< -o $@

# Debug build
$(DEBUGDIR)/$(TARGET): $(OBJS_DEBUG) | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS)

$(DEBUGDIR)/$(TARGET_TPACKET): $(SRC_TPACKET) | $(DEBUGDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $< $(LDFLAGS_TPACKET)

$(OBJDIR_D)/%.o: %.c | $(OBJDIR_D)/.dir
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

# Unit Tests
$(TESTDIR)/$(TEST_TARGET): tests/test_filter.c filter.c packet.c | $(TESTDIR)/.dir
	$(CC) $(DEBUG_CFLAGS) -o $@ $^ $(LDFLAGS)

run-tests: tests
	./$(TESTDIR)/$(TEST_TARGET)

clean:
	rm -rf $(RELEASEDIR) $(DEBUGDIR) $(TESTDIR)

.PHONY: all release debug tests run-tests clean
# .dir 마커 파일이 intermediate로 분류되어 자동 삭제되는 것을 방지
.PRECIOUS: %/.dir
