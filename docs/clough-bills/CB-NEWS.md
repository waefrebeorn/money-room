# CB-NEWS — Financial News Pipeline

**Priority:** ⚪ P5
**Cell:** P30 (3 points)
**Language:** C

## Data Source
- **GDELT Project** (already built — CB-P7)
- **RSS Feeds** (free, no API key): Yahoo Finance RSS, MarketWatch, Seeking Alpha, Benzinga
- **NewsAPI** free tier (100 req/day): optional enhancement

## Pipeline (C binary)
```
./news_collector
├── libcurl → GET RSS feeds (multiple sources)
├── Parse XML (libxml2 or manual): title, link, date, source
├── Classify by ticker (ticker mention extraction via regex)
├── Score sentiment (lexicon-based, same as GDELT)
├── Deduplicate (same headline across sources)
├── SQLite → ~/.hermes/news_cache/news.db
└── cron: every 30min (RSS is real-time)
```

### Schema
```sql
CREATE TABLE news_headlines (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT,
    title TEXT, url TEXT,
    source TEXT, published_at TIMESTAMP,
    sentiment REAL,  -- -1.0 to 1.0
    ticker_mentioned TEXT,  -- comma-separated
    is_market_news INTEGER, -- general market vs single ticker
    source_category TEXT    -- 'mainstream', 'blog', 'press_release'
);

CREATE TABLE news_aggregate (
    ticker TEXT PRIMARY KEY,
    headline_count_24h INTEGER,
    avg_sentiment_24h REAL,
    pos_count INTEGER, neg_count INTEGER,
    dominant_source TEXT,
    news_signal TEXT -- 'positive', 'negative', 'mixed', 'quiet'
);
```

## MCP Tools
- `get_news_headlines(ticker=None, limit=20)` — financial headlines
- `get_news_sentiment(ticker)` — 24h aggregate sentiment score

### Works with GDELT
GDELT gives global news sentiment at the country/market level. RSS gives ticker-level individual headlines. Together they cover both macro and micro news.

### Pitfalls
- RSS feeds are fragile — XML structure changes without notice
- Ticker extraction via regex is noisy (e.g. "A" vs "A" in text)
- 3+ upstream RSS sources for redundancy
- NewsAPI free tier only 100 req/day — use as fallback only
