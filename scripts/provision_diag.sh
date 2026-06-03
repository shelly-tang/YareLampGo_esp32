#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
# SPDX-License-Identifier: GPL-3.0-only

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

AP_IP="${LAMPGO_AP_IP:-192.168.4.1}"
LAMPGO_URL="${LAMPGO_URL:-http://127.0.0.1:8420}"
OUT_FILE=""

usage() {
  cat <<'EOF'
LampGo ESP32 provisioning diagnostic

Run this while the computer is connected to Lampgo-Setup-XXXX.
It writes a log that can be shared after switching back to normal WiFi.

Usage:
  scripts/provision_diag.sh [options]

Options:
  --ap-ip IP          ESP32 SoftAP IP, default 192.168.4.1.
  --lampgo-url URL    LampGo backend URL, default http://127.0.0.1:8420.
  --out FILE          Write log to FILE instead of logs/provision-diagnostic-*.log.
  -h, --help          Show this help.

Examples:
  ./scripts/provision_diag.sh
  ./scripts/provision_diag.sh --lampgo-url http://127.0.0.1:8420
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --ap-ip)
      [ "$#" -ge 2 ] || { echo "--ap-ip needs a value" >&2; exit 2; }
      AP_IP="$2"
      shift 2
      ;;
    --lampgo-url)
      [ "$#" -ge 2 ] || { echo "--lampgo-url needs a value" >&2; exit 2; }
      LAMPGO_URL="${2%/}"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || { echo "--out needs a value" >&2; exit 2; }
      OUT_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [ -z "$OUT_FILE" ]; then
  mkdir -p "$ROOT_DIR/logs"
  OUT_FILE="$ROOT_DIR/logs/provision-diagnostic-$(date +%Y%m%d-%H%M%S).log"
else
  mkdir -p "$(dirname "$OUT_FILE")"
fi

exec > >(tee "$OUT_FILE") 2>&1

section() {
  printf '\n========== %s ==========\n' "$*"
}

run() {
  section "$*"
  "$@"
  local rc=$?
  printf '\n[exit %s]\n' "$rc"
  return 0
}

run_sh() {
  section "$*"
  bash -c "$*"
  local rc=$?
  printf '\n[exit %s]\n' "$rc"
  return 0
}

run_redacted_sh() {
  section "$*"
  bash -c "$*" | sed -E 's#([a-zA-Z][a-zA-Z0-9+.-]*://)[^/@[:space:]]+@#\1***@#g'
  local rc=${PIPESTATUS[0]}
  printf '\n[exit %s]\n' "$rc"
  return 0
}

curl_get() {
  local label="$1"
  local url="$2"
  local max_time="$3"
  section "$label: GET $url"
  curl -sS -v --connect-timeout 3 --max-time "$max_time" "$url"
  local rc=$?
  printf '\n[exit %s]\n' "$rc"
  return 0
}

curl_post_json() {
  local label="$1"
  local url="$2"
  local json="$3"
  local max_time="$4"
  section "$label: POST $url"
  printf 'request: %s\n' "$json"
  curl -sS -v --connect-timeout 3 --max-time "$max_time" \
    -H 'Content-Type: application/json' \
    --data "$json" \
    "$url"
  local rc=$?
  printf '\n[exit %s]\n' "$rc"
  return 0
}

detect_python() {
  if [ -n "${LAMPGO_PYTHON:-}" ]; then
    printf '%s\n' "$LAMPGO_PYTHON"
    return 0
  fi
  if [ -x "$ROOT_DIR/../lampgo/.venv/bin/python" ]; then
    printf '%s\n' "$ROOT_DIR/../lampgo/.venv/bin/python"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    command -v python3
  fi
}

python_httpx_get() {
  local label="$1"
  local url="$2"
  local trust_env="$3"
  local timeout="$4"
  section "$label: Python httpx GET $url trust_env=$trust_env"
  local python_bin="${PYTHON_BIN:-}"
  if [ -z "$python_bin" ]; then
    echo "skip: python not found"
    printf '\n[exit 0]\n'
    return 0
  fi
  "$python_bin" - "$url" "$trust_env" "$timeout" <<'PY'
import sys

url = sys.argv[1]
trust_env = sys.argv[2].lower() == "true"
timeout = float(sys.argv[3])

try:
    import httpx
except Exception as exc:
    print(f"skip: cannot import httpx: {type(exc).__name__}: {exc}")
    raise SystemExit(0)

try:
    with httpx.Client(timeout=timeout, trust_env=trust_env) as client:
        resp = client.get(url)
    print(f"ok status={resp.status_code} content_type={resp.headers.get('content-type', '')}")
    print(resp.text[:1000])
except Exception as exc:
    detail = f"{type(exc).__name__}: {exc}" if str(exc) else type(exc).__name__
    print(f"error {detail}")
PY
  local rc=$?
  printf '\n[exit %s]\n' "$rc"
  return 0
}

detect_macos_wifi_device() {
  networksetup -listallhardwareports 2>/dev/null | awk '
    /Hardware Port: Wi-Fi|Hardware Port: AirPort/ { found=1; next }
    found && /^Device:/ { print $2; exit }
  '
}

detect_macos_wifi_service() {
  networksetup -listallhardwareports 2>/dev/null | awk '
    /Hardware Port: Wi-Fi|Hardware Port: AirPort/ {
      sub(/^Hardware Port: /, "", $0)
      print $0
      exit
    }
  '
}

echo "LampGo ESP32 provisioning diagnostic"
echo "Log file: $OUT_FILE"
echo "Timestamp: $(date)"
echo "AP IP: $AP_IP"
echo "LampGo URL: $LAMPGO_URL"
echo
echo "Important: run this while connected to Lampgo-Setup-XXXX."

PYTHON_BIN="$(detect_python || true)"
if [ -n "${PYTHON_BIN:-}" ]; then
  echo "Python for httpx checks: $PYTHON_BIN"
else
  echo "Python for httpx checks: not found"
fi

section "System"
uname -a
date

if command -v networksetup >/dev/null 2>&1; then
  WIFI_SERVICE="$(detect_macos_wifi_service)"
  [ -n "${WIFI_SERVICE:-}" ] || WIFI_SERVICE="Wi-Fi"
  WIFI_DEV="$(detect_macos_wifi_device)"
  [ -n "${WIFI_DEV:-}" ] || WIFI_DEV="en0"
  echo "Detected macOS WiFi service: $WIFI_SERVICE"
  echo "Detected macOS WiFi device: $WIFI_DEV"
  run networksetup -getairportnetwork "$WIFI_DEV"
  run ipconfig getifaddr "$WIFI_DEV"
  run_sh "ipconfig getpacket '$WIFI_DEV' | sed -n '1,180p'"
else
  WIFI_SERVICE=""
  WIFI_DEV=""
fi

section "Proxy environment"
run_redacted_sh "env | sort | grep -Ei '(^|_)(http|https|all|no)_proxy=' || true"
if command -v launchctl >/dev/null 2>&1; then
  run_redacted_sh "for k in HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY http_proxy https_proxy all_proxy no_proxy; do v=\$(launchctl getenv \$k); [ -n \"\$v\" ] && printf '%s=%s\\n' \"\$k\" \"\$v\"; done"
fi
if command -v scutil >/dev/null 2>&1; then
  run_redacted_sh "scutil --proxy"
fi
if command -v networksetup >/dev/null 2>&1 && [ -n "${WIFI_SERVICE:-}" ]; then
  run networksetup -getwebproxy "$WIFI_SERVICE"
  run networksetup -getsecurewebproxy "$WIFI_SERVICE"
  run networksetup -getsocksfirewallproxy "$WIFI_SERVICE"
  run networksetup -getproxybypassdomains "$WIFI_SERVICE"
fi

if command -v scutil >/dev/null 2>&1; then
  run_sh "scutil --nwi | sed -n '1,180p'"
fi

if command -v ifconfig >/dev/null 2>&1; then
  run_sh "ifconfig | sed -n '1,260p'"
fi

if command -v route >/dev/null 2>&1; then
  run route -n get "$AP_IP"
fi

if command -v netstat >/dev/null 2>&1; then
  run_sh "netstat -rn | sed -n '1,160p'"
fi

if command -v arp >/dev/null 2>&1; then
  run_sh "arp -an | grep -E '$AP_IP|192\\.168\\.4|10\\.220' || true"
fi

if command -v ip >/dev/null 2>&1; then
  run ip addr
  run ip route get "$AP_IP"
fi

if command -v iwgetid >/dev/null 2>&1; then
  run iwgetid
fi

section "LampGo backend process hints"
run_sh "ps aux | grep -E 'lampgo|uvicorn|python' | grep -v grep | sed -n '1,120p' || true"

curl_get "Direct ESP32 status" "http://$AP_IP/status" 8
python_httpx_get "Direct ESP32 status" "http://$AP_IP/status" true 8
python_httpx_get "Direct ESP32 status" "http://$AP_IP/status" false 8
curl_get "Direct ESP32 scan" "http://$AP_IP/scan" 25
python_httpx_get "Direct ESP32 scan" "http://$AP_IP/scan" false 25

curl_get "LampGo device status" "$LAMPGO_URL/api/device/status" 8
curl_post_json "LampGo probe ESP32 status" "$LAMPGO_URL/api/device/probe" \
  "{\"base_url\":\"http://$AP_IP\",\"path\":\"/status\",\"method\":\"GET\",\"body\":null}" 12
curl_post_json "LampGo probe ESP32 scan" "$LAMPGO_URL/api/device/probe" \
  "{\"base_url\":\"http://$AP_IP\",\"path\":\"/scan\",\"method\":\"GET\",\"body\":null}" 30

section "How to read this log"
cat <<EOF
1. If Direct ESP32 status times out:
   The Mac is not actually able to reach the SoftAP HTTP server.
   Check the WiFi SSID, DHCP address, route to $AP_IP, and ESP32 serial boot logs.

2. If Direct ESP32 status works but LampGo probe fails:
   The ESP32 is reachable from the Mac, but the LampGo backend cannot proxy it.
   Check LampGo URL/port, backend process, or whether LampGo runs in a different host/container.
   If Python httpx only works with trust_env=false, the backend should bypass proxy env for ESP32 URLs.

3. If status works but scan fails with scan_result < 0:
   The ESP32 provisioning HTTP server is alive, but WiFi scanning failed inside firmware.
   Send the scan_result value and serial logs.

4. If scan returns networks but the UI still says scanning failed:
   The problem is likely in the LampGo frontend/backend probe path, not the ESP32 AP.
EOF

echo
echo "Done. Share this log after reconnecting to normal WiFi:"
echo "$OUT_FILE"
