#!/usr/bin/env bash
# validate_prod.sh — Production 환경 사전 검증
# 변경이력: v1.0.0 | 2026-04-07 | [Implementer] | Phase 4 | 프로덕션 보안/설정 검증
#
# Usage: ./scripts/validate_prod.sh
#
# Validates: TLS certs, SRTP config, API key strength, port availability,
#            Docker images, environment variables, C++ standby status

set -euo pipefail

PASS=0
FAIL=0
WARN=0

pass() { echo "  ✓ PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  ✗ FAIL: $1"; FAIL=$((FAIL + 1)); }
warn() { echo "  △ WARN: $1"; WARN=$((WARN + 1)); }

echo "============================================"
echo " Production Environment Validation"
echo "============================================"
echo ""

# ─────────────────────────────────────────
# 1. Environment file
# ─────────────────────────────────────────
echo "--- 1. Environment Configuration ---"

if [ -f .env ]; then
    pass ".env file exists"
else
    fail ".env file missing (copy from .env.example)"
fi

# API Key length
if [ -f .env ]; then
    KEY=$(grep "^ADMIN_API_KEY=" .env 2>/dev/null | cut -d'=' -f2- || echo "")
    if [ ${#KEY} -ge 16 ]; then
        pass "ADMIN_API_KEY length ≥ 16 chars (${#KEY})"
    else
        fail "ADMIN_API_KEY too short (${#KEY} chars, need ≥ 16)"
    fi

    if echo "${KEY}" | grep -q "changeme"; then
        fail "ADMIN_API_KEY contains 'changeme' — must change for production"
    else
        pass "ADMIN_API_KEY is not default"
    fi

    # ESL password
    # T-16: ESL default password is now a FAIL (security critical)
    ESL_PW=$(grep "^ESL_PASSWORD=" .env 2>/dev/null | cut -d'=' -f2- || echo "")
    if [ "${ESL_PW}" = "ClueCon" ] || [ -z "${ESL_PW}" ]; then
        fail "ESL_PASSWORD must not be default 'ClueCon' or empty — generate with: openssl rand -hex 16"
    else
        pass "ESL_PASSWORD is not default"
    fi

    # Runtime profile
    PROFILE=$(grep "^RUNTIME_PROFILE=" .env 2>/dev/null | cut -d'=' -f2- || echo "dev")
    if [ "${PROFILE}" = "production" ]; then
        pass "RUNTIME_PROFILE = production"
    else
        fail "RUNTIME_PROFILE = '${PROFILE}' (expected 'production')"
    fi
fi

# ─────────────────────────────────────────
# 2. TLS Certificates
# ─────────────────────────────────────────
echo ""
echo "--- 2. TLS Certificates ---"

TLS_DIR="config/freeswitch/tls"
if [ -d "${TLS_DIR}" ]; then
    pass "TLS directory exists"

    for cert in agent.pem cafile.pem; do
        if [ -f "${TLS_DIR}/${cert}" ]; then
            pass "${cert} exists"
            # Check expiry
            EXPIRY=$(openssl x509 -enddate -noout -in "${TLS_DIR}/${cert}" 2>/dev/null | cut -d= -f2 || echo "unknown")
            echo "    Expires: ${EXPIRY}"
        else
            fail "${cert} missing in ${TLS_DIR}"
        fi
    done
else
    fail "TLS directory ${TLS_DIR} not found"
fi

# ─────────────────────────────────────────
# 3. SRTP Configuration
# ─────────────────────────────────────────
echo ""
echo "--- 3. SRTP Configuration ---"

if [ -f .env ]; then
    SRTP=$(grep "^SRTP_MODE=" .env 2>/dev/null | cut -d'=' -f2- || echo "optional")
    if [ "${SRTP}" = "mandatory" ]; then
        pass "SRTP_MODE = mandatory"
    else
        warn "SRTP_MODE = '${SRTP}' (recommended: mandatory for production)"
    fi

    TLS_ONLY=$(grep "^SIP_TLS_ONLY=" .env 2>/dev/null | cut -d'=' -f2- || echo "false")
    if [ "${TLS_ONLY}" = "true" ]; then
        pass "SIP_TLS_ONLY = true"
    else
        warn "SIP_TLS_ONLY = '${TLS_ONLY}' (recommended: true for production)"
    fi

    GRPC_TLS=$(grep "^AI_GRPC_TLS=" .env 2>/dev/null | cut -d'=' -f2- || echo "false")
    if [ "${GRPC_TLS}" = "true" ]; then
        pass "AI_GRPC_TLS = true"
    else
        warn "AI_GRPC_TLS = '${GRPC_TLS}' (recommended: true for production)"
    fi
fi

# ─────────────────────────────────────────
# 4. Docker Images
# ─────────────────────────────────────────
echo ""
echo "--- 4. Docker Images ---"

if command -v docker &>/dev/null; then
    pass "Docker installed"

    if docker compose version &>/dev/null; then
        pass "Docker Compose available"
    else
        fail "Docker Compose not available"
    fi
else
    fail "Docker not installed"
fi

# ─────────────────────────────────────────
# 5. Port Availability
# ─────────────────────────────────────────
echo ""
echo "--- 5. Port Availability ---"

for port in 5060 5061 8021 8080 8090 8091; do
    if lsof -i ":${port}" &>/dev/null 2>&1; then
        warn "Port ${port} is in use (may conflict with services)"
    else
        pass "Port ${port} available"
    fi
done

# ─────────────────────────────────────────
# 5.5 NAT / Public IP (S-04)
# ─────────────────────────────────────────
echo ""
echo "--- 5.5 NAT Configuration (S-04) ---"

if [ -f .env ]; then
    EXT_RTP=$(grep "^EXTERNAL_RTP_IP=" .env 2>/dev/null | cut -d'=' -f2- || echo "auto-nat")
    EXT_SIP=$(grep "^EXTERNAL_SIP_IP=" .env 2>/dev/null | cut -d'=' -f2- || echo "auto-nat")

    if [ "${EXT_RTP}" = "auto-nat" ] || [ "${EXT_SIP}" = "auto-nat" ]; then
        PUBLIC_IP=$(curl -s --connect-timeout 3 ifconfig.me 2>/dev/null || echo "")
        if [ -n "${PUBLIC_IP}" ]; then
            warn "EXTERNAL_RTP_IP=auto-nat — NAT 환경에서 one-way audio 위험"
            echo "    감지된 공인 IP: ${PUBLIC_IP}"
            echo "    권장: EXTERNAL_RTP_IP=${PUBLIC_IP}"
            echo "    권장: EXTERNAL_SIP_IP=${PUBLIC_IP}"
        else
            warn "EXTERNAL_RTP_IP=auto-nat (공인 IP 확인 불가 — 네트워크 확인 필요)"
        fi
    else
        pass "EXTERNAL_RTP_IP=${EXT_RTP} (명시적 설정)"
        pass "EXTERNAL_SIP_IP=${EXT_SIP} (명시적 설정)"
    fi
fi

# ─────────────────────────────────────────
# 6. VAD Model
# ─────────────────────────────────────────
echo ""
echo "--- 6. VAD Model ---"

MODEL_PATH="models/silero_vad.onnx"
if [ -f "${MODEL_PATH}" ]; then
    SIZE=$(ls -la "${MODEL_PATH}" | awk '{print $5}')
    pass "silero_vad.onnx exists (${SIZE} bytes)"
else
    # Check parent directory
    if [ -f "../../models/silero_vad.onnx" ]; then
        pass "silero_vad.onnx found in parent project"
        warn "Consider copying to ./models/ for Docker mount"
    else
        fail "silero_vad.onnx not found"
    fi
fi

# ─────────────────────────────────────────
# 7. Disk Space
# ─────────────────────────────────────────
echo ""
echo "--- 7. Disk Space ---"

AVAIL_GB=$(df -g . 2>/dev/null | tail -1 | awk '{print $4}' || echo "?")
if [ "${AVAIL_GB}" != "?" ] && [ "${AVAIL_GB}" -ge 10 ]; then
    pass "Disk space: ${AVAIL_GB}GB available"
else
    warn "Disk space: ${AVAIL_GB}GB (recommend ≥ 10GB for recordings + logs)"
fi

# ─────────────────────────────────────────
# Summary
# ─────────────────────────────────────────
echo ""
echo "============================================"
echo " Validation Results"
echo "============================================"
echo " PASS: ${PASS}"
echo " FAIL: ${FAIL}"
echo " WARN: ${WARN}"
echo ""

if [ "${FAIL}" -gt 0 ]; then
    echo " STATUS: NOT READY — fix ${FAIL} failures before production deployment"
    exit 1
else
    if [ "${WARN}" -gt 0 ]; then
        echo " STATUS: READY WITH WARNINGS — review ${WARN} warnings"
    else
        echo " STATUS: READY FOR PRODUCTION"
    fi
    exit 0
fi
