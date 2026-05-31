#!/bin/bash
# register_processor.sh — Process pending registrations, add keys, clean expired
~/money-room/engine/register_processor 2>&1 || echo "[REG] processor failed"
# Then git add and push the updated .auth.json
cd ~/money-room
git add docs/.auth.json
if ! git diff --cached --quiet; then
  git commit -m "auth: key updates $(date +'%Y-%m-%d %H:%M')"
  git push origin main 2>&1
  echo "[REG] Pushed auth updates"
fi
