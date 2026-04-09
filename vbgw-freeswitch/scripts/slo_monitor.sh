#!/usr/bin/env bash
# slo_monitor.sh — SLO 실시간 모니터링 (health uptime + active calls + errors)
# 변경이력: v1.0.0 | 2026-04-07 | [Implementer] | Phase 3 | SLO 측정
#
# Usage: ./scripts/slo_monitor.sh [TARGET_IP] [OUTPUT_CSV]
# Runs until killed (SIGTERM/SIGINT)

set -euo pipefail

TARGET_IP="${1:-127.0.0.1}"
OUTPUT_CSV="${2:-slo_log.csv}"
INTERVAL=5  # seconds

echo "timestamp,health_status,active_calls,dropped_frames,stream_errors,latency_ms" > "${OUTPUT_CSV}"

while true; do
    TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    START_MS=$(date +%s%3N 2>/dev/null || python3 -c "import time; print(int(time.time()*1000))")

    # T-25: Isolate curl failures from set -e (prevent monitor exit on transient network error)
    HEALTH_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://${TARGET_IP}:8080/health" 2>/dev/null) || HEALTH_CODE="000"
    END_MS=$(date +%s%3N 2>/dev/null || python3 -c "import time; print(int(time.time()*1000))")
    LATENCY=$((END_MS - START_MS))

    # Metrics — T-25: Isolate curl failures
    METRICS=$(curl -s "http://${TARGET_IP}:8080/metrics" 2>/dev/null) || METRICS=""
    ACTIVE=$(echo "${METRICS}" | grep "^vbgw_active_calls " | awk '{print $2}' || echo "0")
    DROPPED=$(echo "${METRICS}" | grep "^vbgw_grpc_dropped_frames_total " | awk '{print $2}' || echo "0")
    ERRORS=$(echo "${METRICS}" | grep "^vbgw_grpc_stream_errors_total " | awk '{print $2}' || echo "0")

    echo "${TIMESTAMP},${HEALTH_CODE},${ACTIVE:-0},${DROPPED:-0},${ERRORS:-0},${LATENCY}" >> "${OUTPUT_CSV}"

    sleep "${INTERVAL}"
done
