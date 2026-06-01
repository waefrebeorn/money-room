CC = gcc
CFLAGS = -O3 -march=native -mtune=native -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=199309L
LDFLAGS = -lm

# All source files
SRCS = room_engine.c room_feeds.c room_features.c room_vote.c room_capital.c room_darwin.c room_bridge.c
PAPER_SRCS = $(SRCS) paper_feature_bridge.c
MKT_SRCS = room_engine.c room_feeds.c room_features.c room_vote.c room_capital.c room_market.c room_darwin.c room_bridge.c
EXCH_SRCS = exchange_api.c
OBJS = $(SRCS:.c=.o)
PAPER_OBJS = $(PAPER_SRCS:.c=.o)
TARGET = room_engine
MKT_TARGET = room_engine_market

.PHONY: all clean run paper market feed_bridge exchange_test tools

# Tools: shell-to-C conversions
TOOLS = cycle_all_rooms room_feed_bridge paper_train collector_runner refresh_all
TOOL_SRCS = $(addsuffix .c, $(TOOLS))
TOOL_BINS = $(TOOLS) cross_asset_c accuracy_scorer health_check health_alerter data_server withdrawal_scheduler test_runner

market: CFLAGS += -DMARKET_MODE
market: clean $(MKT_TARGET)

$(MKT_TARGET): $(MKT_SRCS:.c=.o)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

paper: CFLAGS += -DPAPER_MODE
paper: clean $(TARGET)_paper

$(TARGET)_paper: $(PAPER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lsqlite3

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c types.h
	$(CC) $(CFLAGS) -c $< -o $@

paper_feature_bridge.o: paper_feature_bridge.c paper_feature_bridge.h types.h
	$(CC) $(CFLAGS) -c paper_feature_bridge.c -o paper_feature_bridge.o

clean:
	rm -f $(OBJS) $(PAPER_OBJS) $(TARGET) $(TARGET)_paper exchange_api.o exchange_api_test room_feed_bridge $(TOOL_BINS)

# ── Shell-to-C conversions ──
tools: cycle_all_rooms room_feed_bridge paper_train collector_runner refresh_all cross_asset_c accuracy_scorer health_check health_alerter data_server withdrawal_scheduler test_runner

cycle_all_rooms: cycle_all_rooms.c
	$(CC) -O2 -o $@ $<

room_feed_bridge: room_feed_bridge.c
	$(CC) -O2 -o $@ $<

paper_train: paper_train.c
	$(CC) -O2 -o $@ $<

collector_runner: collector_runner.c
	$(CC) -O2 -o $@ $<

refresh_all: refresh_all.c
	$(CC) -O2 -o $@ $< -lsqlite3

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

# ── Multi-market trainer + collectors ──
multi_market_trainer: multi_market_trainer.c types.h
	$(CC) -O3 -o multi_market_trainer multi_market_trainer.c -lsqlite3 -ljansson -lm -I.

weather_collector: weather_collector.c
	$(CC) -O2 -o weather_collector weather_collector.c -lcurl -ljansson -lm

sports_collector: sports_collector.c
	$(CC) -O2 -o sports_collector sports_collector.c -lcurl -ljansson -lm

all_collectors: weather_collector sports_collector multi_market_trainer

# ── Cross-asset correlation engine (reads timeline.db) ──
cross_asset_c: cross_asset_c.c
	$(CC) -O2 -o $@ $< $(LDFLAGS) -lsqlite3 -lm

cross: cross_asset_c
	@./cross_asset_c

# ── Accuracy scorer (reads outcomes.db) ──
accuracy_scorer: accuracy_scorer.c
	$(CC) -O2 -o $@ $< $(LDFLAGS) -lsqlite3 -lm

# ── Health endpoint (libjansson) ──
health_check: health_check.c
	$(CC) -O2 -o $@ $< -ljansson

# ── Health alerter (alerts on health_check failure) ──
health_alerter: health_alerter.c
	$(CC) -O2 -o $@ $< -ljansson

# ── Data server (static file server for docs/data/) ──
data_server: data_server.c
	$(CC) -O2 -o $@ $<

# ── Withdrawal scheduler (SQLite-backed) ──
withdrawal_scheduler: withdrawal_scheduler.c
	$(CC) -O3 -march=native -o $@ $< -lsqlite3 -lm

# ── Test runner (integration test harness) ──
test_runner: test_runner.c
	$(CC) -O2 -o $@ $< -lm

# ── Run test suite ──
test: test_runner
	@./test_runner
