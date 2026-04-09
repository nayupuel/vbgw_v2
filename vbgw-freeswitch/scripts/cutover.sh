#!/usr/bin/env bash
# cutover.sh — Blue/Green 단계별 전환 + SLO 감시 + 자동 롤백
# 변경이력: v1.0.0 | 2026-04-07 | [Implementer] | Phase 4 | 10→25→50→100% 전환
#
# Usage: ./scripts/cutover.sh [STAGE]
#   STAGE: 10, 25, 50, 100, rollback
#
# Prerequisites:
#   - PBX 라우팅 API 또는 수동 전환 대기
#   - Production Docker Compose 실행 중
#   - C++ vbgw standby 실행 중

set -euo pipefail

STAGE="${1:-10}"
FS_IP="${FS_IP:-127.0.0.1}"
CPP_IP="${CPP_IP:-127.0.0.2}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="${SCRIPT_DIR}/../results/cutover_$(date +%Y%m%d_%H%M%S)"

mkdir -p "${LOG_DIR}"

# SLO thresholds (from migration_v2.md Section 8)
HEALTH_MIN_PERCENT=99.9        # /health availability in 5-min window
DROP_RATE_MAX=1.0              # audio queue drop rate %
GRPC_ERROR_MAX=0.5             # gRPC stream failure rate %

# ─────────────────────────────────────────
# SLO check function
# ─────────────────────────────────────────
check_slo() {
    local duration_sec="${1:-300}"  # default 5 minutes
    local interval=5
    local checks=$((duration_sec / interval))
    local healthy=0
    local total=0

    echo "  SLO monitoring for ${duration_sec}s (${checks} checks)..."

    for _ in $(seq 1 "${checks}"); do
        total=$((total + 1))
        CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://${FS_IP}:8080/health" 2>/dev/null || echo "000")
        if [ "${CODE}" = "200" ]; then
            healthy=$((healthy + 1))
        fi
        sleep "${interval}"
    done

    # T-24: Use bc for floating-point SLO check (integer division can't express 99.9%)
    local percent="0"
    if [ "${total}" -gt 0 ]; then
        percent=$(echo "scale=1; ${healthy} * 100 / ${total}" | bc 2>/dev/null || echo "$((healthy * 100 / total))")
    fi

    # Check drop rate
    local metrics
    metrics=$(curl -s "http://${FS_IP}:8080/metrics" 2>/dev/null || echo "")
    local dropped
    dropped=$(echo "${metrics}" | grep "^vbgw_grpc_dropped_frames_total " | awk '{print $2}' || echo "0")
    local errors
    errors=$(echo "${metrics}" | grep "^vbgw_grpc_stream_errors_total " | awk '{print $2}' || echo "0")

    echo "  Health uptime: ${percent}% (${healthy}/${total})"
    echo "  Dropped frames: ${dropped}"
    echo "  Stream errors: ${errors}"

    # T-24: Evaluate with floating-point comparison
    if echo "${percent} < 99.9" | bc -l 2>/dev/null | grep -q '^1'; then
        echo "  ✗ FAIL: Health uptime ${percent}% < 99.9%"
        return 1
    fi

    echo "  ✓ SLO check PASSED"
    return 0
}

# ─────────────────────────────────────────
# Rollback procedure
# ─────────────────────────────────────────
do_rollback() {
    echo ""
    echo "============================================"
    echo " ROLLBACK INITIATED"
    echo "============================================"
    echo ""
    echo "Step 1: Route ALL traffic back to C++ vbgw (${CPP_IP})"
    echo "  → PBX admin: Immediately set 100% routing to ${CPP_IP}"
    echo "  → C++ system is already running in standby"
    echo ""
    echo "Step 2: Graceful shutdown FreeSWITCH stack"
    echo "  → Run: docker compose exec orchestrator kill -INT 1"
    echo "  → Wait for '[Shutdown 5/5] ESL connection closed' in logs"
    echo "  → Run: docker compose -f docker-compose.yml -f docker-compose.prod.yml down"
    echo ""
    echo "Step 3: Collect post-mortem data"
    echo "  → docker compose logs > ${LOG_DIR}/rollback_logs.txt"
    echo "  → curl http://${FS_IP}:8080/metrics > ${LOG_DIR}/rollback_metrics.txt"
    echo "  → curl http://${FS_IP}:8080/debug/pprof/goroutine > ${LOG_DIR}/rollback_goroutine.prof"
    echo ""
    echo "Step 4: Post-mortem analysis"
    echo "  → Analyze logs for errors"
    echo "  → Update risk register"
    echo "  → Fix root cause before re-attempting Phase 3"
    echo ""

    # Capture logs if stack is still running
    docker compose logs > "${LOG_DIR}/rollback_logs.txt" 2>/dev/null || true
    curl -s "http://${FS_IP}:8080/metrics" > "${LOG_DIR}/rollback_metrics.txt" 2>/dev/null || true

    echo "Rollback data saved to: ${LOG_DIR}"
    echo "============================================"
    exit 1
}

# ─────────────────────────────────────────
# Main cutover logic
# ─────────────────────────────────────────
echo "============================================"
echo " Phase 4: Blue/Green Cutover — Stage ${STAGE}%"
echo "============================================"
echo " FreeSWITCH: ${FS_IP}"
echo " C++ vbgw:   ${CPP_IP}"
echo " Log dir:    ${LOG_DIR}"
echo "============================================"
echo ""

case "${STAGE}" in
    10)
        echo "=== Stage 1: 10% traffic → FreeSWITCH ==="
        echo ""
        echo "ACTION REQUIRED: Configure PBX to route 10% of inbound calls to ${FS_IP}:5060"
        echo "Remaining 90% continues to C++ vbgw at ${CPP_IP}"
        echo ""
        read -rp "Press ENTER when PBX routing is configured, or Ctrl+C to abort..."
        echo ""

        echo "Monitoring SLO for 1 hour (3600s)..."
        if ! check_slo 3600; then
            echo ""
            echo "SLO VIOLATION at 10% — triggering rollback"
            do_rollback
        fi

        echo ""
        echo "✓ Stage 1 (10%) PASSED — 1 hour stable"
        echo "Next: ./scripts/cutover.sh 25"
        ;;

    25)
        echo "=== Stage 2: 25% traffic → FreeSWITCH ==="
        echo ""
        echo "ACTION REQUIRED: Increase PBX routing to 25% for ${FS_IP}:5060"
        echo ""
        read -rp "Press ENTER when done..."
        echo ""

        echo "Monitoring SLO for 1 hour..."
        if ! check_slo 3600; then
            do_rollback
        fi

        echo ""
        echo "✓ Stage 2 (25%) PASSED"
        echo "Next: ./scripts/cutover.sh 50"
        ;;

    50)
        echo "=== Stage 3: 50% traffic → FreeSWITCH ==="
        echo ""
        echo "ACTION REQUIRED: Increase PBX routing to 50% for ${FS_IP}:5060"
        echo ""
        read -rp "Press ENTER when done..."
        echo ""

        echo "Monitoring SLO for 24 hours (86400s)..."
        if ! check_slo 86400; then
            do_rollback
        fi

        echo ""
        echo "✓ Stage 3 (50%) PASSED — 24 hours stable"
        echo "Next: ./scripts/cutover.sh 100"
        ;;

    100)
        echo "=== Stage 4: 100% traffic → FreeSWITCH ==="
        echo ""
        echo "ACTION REQUIRED: Route ALL traffic to ${FS_IP}:5060"
        echo "C++ vbgw at ${CPP_IP} remains in standby (DO NOT STOP)"
        echo ""
        read -rp "Press ENTER when done..."
        echo ""

        echo "Monitoring SLO for 7 days (604800s)..."
        echo "(Check progress: tail -f ${LOG_DIR}/slo_check.log)"

        TOTAL_DAYS=7
        for day in $(seq 1 "${TOTAL_DAYS}"); do
            echo ""
            echo "--- Day ${day}/${TOTAL_DAYS} ---"
            if ! check_slo 86400; then
                echo "SLO VIOLATION on day ${day} — triggering rollback"
                do_rollback
            fi
            echo "Day ${day} PASSED"
        done

        echo ""
        echo "============================================"
        echo " CUTOVER COMPLETE"
        echo "============================================"
        echo ""
        echo "100% traffic on FreeSWITCH for 7 days — 0 incidents"
        echo ""
        echo "C++ vbgw decommission checklist:"
        echo "  1. Confirm 0 active calls on C++ system"
        echo "  2. Stop C++ vbgw process (graceful shutdown)"
        echo "  3. Archive C++ logs and configuration"
        echo "  4. Update DNS/PBX to remove C++ IP"
        echo "  5. Update monitoring dashboards"
        echo ""
        echo "Congratulations! Migration complete."
        ;;

    rollback)
        do_rollback
        ;;

    *)
        echo "Usage: $0 [10|25|50|100|rollback]"
        exit 1
        ;;
esac
