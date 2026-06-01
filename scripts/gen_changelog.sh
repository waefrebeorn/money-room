#!/bin/bash
# gen_changelog.sh — Generate CHANGELOG.md from git history
# Usage: ./gen_changelog.sh [since date] [output file]
# Default: 30 days ago, docs/CHANGELOG.md

SINCE="${1:-2026-05-01}"
OUTPUT="${2:-docs/CHANGELOG.md}"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR" || exit 1

cat > "$OUTPUT" << 'HEADER'
# 📋 Changelog — Money Room

Auto-generated from git history. Updated on each deploy.

| Date | Commit | Description |
|------|--------|-------------|
HEADER

git log --since="$SINCE" --format="| %cd | %h | %s |" --date=format:"%Y-%m-%d %H:%M" >> "$OUTPUT"

echo "" >> "$OUTPUT"
echo "_Generated $(date '+%Y-%m-%d %H:%M UTC')_" >> "$OUTPUT"

echo "[CHANGELOG] Written to $OUTPUT ($(wc -l < "$OUTPUT") lines)"
