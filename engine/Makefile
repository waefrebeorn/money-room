CC = gcc
CFLAGS = -O3 -march=native -mtune=native -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=199309L
LDFLAGS = -lm

# All source files
SRCS = room_engine.c room_feeds.c room_features.c room_vote.c room_capital.c room_darwin.c room_bridge.c
MKT_SRCS = room_engine.c room_feeds.c room_features.c room_vote.c room_capital.c room_market.c room_darwin.c room_bridge.c
EXCH_SRCS = exchange_api.c
OBJS = $(SRCS:.c=.o)
TARGET = room_engine
MKT_TARGET = room_engine_market

.PHONY: all clean run paper market feed_bridge exchange_test

market: CFLAGS += -DMARKET_MODE
market: clean $(MKT_TARGET)

$(MKT_TARGET): $(MKT_SRCS:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

paper: CFLAGS += -DPAPER_MODE
paper: clean $(TARGET)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c types.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) exchange_api.o exchange_api_test room_feed_bridge

run: $(TARGET)
	./$(TARGET)

stock: stock_collector.c
	$(CC) -O3 -o stock_collector stock_collector.c -lcurl -ljansson -lsqlite3 -lm

# ── T11: Exchange API module ──
exchange_api.o: exchange_api.c exchange_api.h
	$(CC) $(CFLAGS) -c exchange_api.c -o exchange_api.o

exchange_api_test: exchange_api_test.c exchange_api.o
	$(CC) $(CFLAGS) -o exchange_api_test exchange_api_test.c exchange_api.o -lcurl -ljansson -lm

# ── T12: Feed bridge with exchange API + WebSocket read ──
feed_bridge: exchange_api.o
	$(CC) -O3 -D_GNU_SOURCE -o room_feed_bridge room_feed_bridge.c exchange_api.o -lcurl -ljansson -lsqlite3 -lm
