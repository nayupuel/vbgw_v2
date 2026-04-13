#!/usr/bin/env bash
set -euo pipefail

# vbgw HTTP Admin API outbound-call soak harness.
# This validates control-plane stability and latency under burst traffic.
#
# Usage:
#   ADMIN_API_KEY=... ./scripts/soak_outbound_api.sh [requests] [concurrency] [target_uri] [admin_base_url]
#
# Example:
#   ADMIN_API_KEY=changeme-admin-key ./scripts/soak_outbound_api.sh 100 10 sip:1001@127.0.0.1 http://127.0.0.1:8080

REQUESTS="${1:-100}"
CONCURRENCY="${2:-10}"
TARGET_URI="${3:-sip:voicebot@127.0.0.1}"
ADMIN_BASE_URL="${4:-http://127.0.0.1:8080}"

if [[ -z "${ADMIN_API_KEY:-}" ]]; then
    echo "ERROR: ADMIN_API_KEY env var is required"
    exit 1
fi

if ! [[ "$REQUESTS" =~ ^[0-9]+$ ]] || ! [[ "$CONCURRENCY" =~ ^[0-9]+$ ]]; then
    echo "ERROR: requests and concurrency must be integers"
    exit 1
fi

if [[ "$REQUESTS" -lt 1 || "$CONCURRENCY" -lt 1 ]]; then
    echo "ERROR: requests and concurrency must be >= 1"
    exit 1
fi

RESULT_FILE="$(mktemp /tmp/vbgw_soak_results.XXXXXX)"
trap 'rm -f "$RESULT_FILE"' EXIT

echo "================================================="
echo "VBGW Outbound API Soak Test"
echo "================================================="
echo "Admin URL    : ${ADMIN_BASE_URL}/api/v1/calls"
echo "Target URI   : ${TARGET_URI}"
echo "Requests     : ${REQUESTS}"
echo "Concurrency  : ${CONCURRENCY}"
echo ""

export ADMIN_API_KEY TARGET_URI ADMIN_BASE_URL RESULT_FILE

seq "$REQUESTS" | xargs -P "$CONCURRENCY" -I{} bash -c '
  result=$(curl -sS -o /dev/null -w "%{http_code} %{time_total}" \
    -X POST \
    -H "X-Admin-Key: ${ADMIN_API_KEY}" \
    -H "Content-Type: application/json" \
    "${ADMIN_BASE_URL}/api/v1/calls" \
    -d "{\"target_uri\":\"${TARGET_URI}\"}" || echo "000 0")
  echo "$result" >> "$RESULT_FILE"
'

total="$(wc -l < "$RESULT_FILE" | tr -d ' ')"
ok="$(awk '$1=="202"{c++} END{print c+0}' "$RESULT_FILE")"
unauth="$(awk '$1=="401" || $1=="403"{c++} END{print c+0}' "$RESULT_FILE")"
throttle="$(awk '$1=="429"{c++} END{print c+0}' "$RESULT_FILE")"
server_err="$(awk '$1 ~ /^5/{c++} END{print c+0}' "$RESULT_FILE")"
avg_sec="$(awk '{s+=$2} END{if(NR>0) printf "%.4f", s/NR; else print "0.0000"}' "$RESULT_FILE")"
p95_sec="$(awk '{print $2}' "$RESULT_FILE" | sort -n | awk '
  {arr[NR]=$1}
  END{
    if(NR==0){print "0.0000"; exit}
    idx=int((NR*95+99)/100)
    if(idx<1) idx=1
    if(idx>NR) idx=NR
    printf "%.4f", arr[idx]
  }'
)"

echo "================================================="
echo "Soak Result"
echo "================================================="
echo "Total requests : ${total}"
echo "202 Accepted   : ${ok}"
echo "401/403 Auth   : ${unauth}"
echo "429 Throttled  : ${throttle}"
echo "5xx Errors     : ${server_err}"
echo "Avg latency(s) : ${avg_sec}"
echo "P95 latency(s) : ${p95_sec}"

if [[ "$ok" -eq 0 ]]; then
    echo ""
    echo "FAIL: no successful outbound call API requests (202)."
    exit 2
fi

echo ""
echo "PASS: outbound API soak run completed."
