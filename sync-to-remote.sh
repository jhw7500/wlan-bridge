#!/usr/bin/env bash
set -euo pipefail

BRANCH=rate-limit-inject-logging
BUNDLE="wlan-bridge.bundle"
REMOTE="root@192.168.0.110"
REMOTE_BUNDLE="/tmp/wlan-bridge.bundle"
REMOTE_DIR="~/wlan-bridge"

echo "[local] create bundle for $BRANCH"
git bundle create "$BUNDLE" "$BRANCH"

echo "[local] copy bundle to $REMOTE:$REMOTE_BUNDLE"
scp "$BUNDLE" "$REMOTE":"$REMOTE_BUNDLE"

echo "[remote] fetch/reset/build"
ssh "$REMOTE" "
  set -euo pipefail
  mkdir -p $REMOTE_DIR
  cd $REMOTE_DIR
  if [ ! -d .git ]; then
    git clone $REMOTE_BUNDLE .
  fi
  git fetch $REMOTE_BUNDLE $BRANCH
  git checkout $BRANCH
  git reset --hard FETCH_HEAD
  make -C dumb
  cp dumb/dumb /usr/local/bin/ -f
"

echo "done."
