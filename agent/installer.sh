#! /usr/bin/env bash
set -e

NGINX_BIN=$(command -v nginx || true)

if [ -z "$NGINX_BIN" ]; then
    echo "[Error] NGINX not found"
    exit 1
fi

NGINX_CONF=$($NGINX_BIN -t 2>&1 \
  | grep "configuration file" \
  | head -n 1 \
  | awk '{print $5}')

echo "[OK] NGINX found at $NGINX_BIN"
echo "[OK] NGINX config at $NGINX_CONF"

INCLUDE_LINE="include /opt/lazyfirewall/config/*.conf;"

# Backup once
if [ ! -f "$NGINX_CONF.bak" ]; then
  cp "$NGINX_CONF" "$NGINX_CONF.bak"
fi

# Check if already injected
if grep -q "/opt/lazyfirewall/config" "$NGINX_CONF"; then
  echo "[OK] LazyFirewall already injected"
  exit 0
fi

echo "[INFO] Injecting LazyFirewall include"

if sed --version >/dev/null 2>&1; then
  # GNU sed (Linux)
  sed -i "/http {/a\\
    $INCLUDE_LINE" "$NGINX_CONF"
else
  # BSD sed (macOS)
  sed -i '' "/http {/a\\
    $INCLUDE_LINE
" "$NGINX_CONF"
fi

echo "[OK] Injection completed"

nginx -t && echo "[OK] NGINX config valid"
