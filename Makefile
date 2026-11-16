CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -g -Ishared -Isrc
LDLIBS  = -pthread
BUILD   = build

SHARED_SRC = shared/engine.c shared/net.c
SHARED_HDR = shared/engine.h shared/protocol.h shared/net.h shared/timing.h
CLI        = src/cli.c src/cli.h

.PHONY: all test clean

all: server client $(BUILD)/test_engine

server: src/server.c $(CLI) $(SHARED_SRC) $(SHARED_HDR)
	$(CC) $(CFLAGS) -o $@ src/server.c src/cli.c $(SHARED_SRC) $(LDLIBS)

client: src/client.c $(CLI) $(SHARED_SRC) $(SHARED_HDR)
	$(CC) $(CFLAGS) -o $@ src/client.c src/cli.c $(SHARED_SRC)

test: $(BUILD)/test_engine
	./$(BUILD)/test_engine

$(BUILD)/test_engine: tests/test_engine.c shared/engine.c shared/engine.h shared/protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_engine.c shared/engine.c

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD) server client
