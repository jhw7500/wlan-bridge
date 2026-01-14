# Makefile for refactored wlan-bridge
# Builds both production binary and unit tests

CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -std=gnu11
LDFLAGS = -lpcap -pthread

# Source files
BRIDGE_SRCS = packet.c filter.c bridge_packet_handler.c
BRIDGE_OBJS = $(BRIDGE_SRCS:.c=.o)

# Test files
TEST_SRCS = tests/test_filter.c
TEST_BINS = $(TEST_SRCS:.c=)

.PHONY: all clean test

all: test_filter

# Unit test executable
test_filter: tests/test_filter.c packet.o filter.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "===================================="
	@echo "  Unit test built: test_filter"
	@echo "  Run with: ./test_filter"
	@echo "===================================="

# Object files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Run tests
test: test_filter
	@echo ""
	@echo "Running unit tests..."
	@./test_filter

# Clean
clean:
	rm -f *.o test_filter
	@echo "Clean complete"

# Help
help:
	@echo "Makefile for refactored wlan-bridge"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build unit test binary (default)"
	@echo "  test        Build and run all tests"
	@echo "  clean       Remove build artifacts"
	@echo "  help        Show this help message"
