#!/usr/bin/env bash
set -euo pipefail

# Validate production env file against vbgw runtime hardening policy.
#
# Usage:
#   ./scripts/validate_prod_env.sh [env_file]
#
# Example:
#   ./scripts/validate_prod_env.sh .env
#
# Optional env:
#   VALIDATE_PROFILE=<prod|production|dev...>     # env file의 VBGW_PROFILE을 덮어써 검증
#   REQUIRE_PRODUCTION_PROFILE=1                  # prod 프로파일이 아니면 실패 처리

ENV_FILE="${1:-.env}"

if [[ ! -f "$ENV_FILE" ]]; then
    echo "ERROR: env file not found: $ENV_FILE"
    exit 1
fi

set -a
# shellcheck source=/dev/null
source "$ENV_FILE"
set +a

errors=0

is_true() {
    local v="${1:-}"
    local normalized
    normalized="$(printf '%s' "$v" | tr '[:upper:]' '[:lower:]')"
    [[ "$normalized" == "1" || "$normalized" == "true" || "$normalized" == "yes" || "$normalized" == "on" ]]
}

is_strong_admin_key() {
    local key="${1:-}"
    [[ "${#key}" -ge 16 ]] &&
        [[ "$key" =~ [A-Z] ]] &&
        [[ "$key" =~ [a-z] ]] &&
        [[ "$key" =~ [0-9] ]] &&
        [[ "$key" =~ [^A-Za-z0-9] ]]
}

require_set() {
    local name="$1"
    local value="${!name:-}"
    if [[ -z "$value" ]]; then
        echo "ERROR: ${name} is required"
        errors=$((errors + 1))
    fi
}

require_file() {
    local name="$1"
    local value="${!name:-}"
    if [[ -z "$value" ]]; then
        echo "ERROR: ${name} is required"
        errors=$((errors + 1))
        return
    fi
    if [[ ! -f "$value" ]]; then
        echo "ERROR: ${name} file not found: $value"
        errors=$((errors + 1))
    fi
}

require_int_range() {
    local name="$1"
    local min="$2"
    local max="$3"
    local value="${!name:-}"

    if [[ -z "$value" ]]; then
        echo "ERROR: ${name} is required"
        errors=$((errors + 1))
        return
    fi
    if ! [[ "$value" =~ ^[0-9]+$ ]]; then
        echo "ERROR: ${name} must be an integer"
        errors=$((errors + 1))
        return
    fi
    if (( value < min || value > max )); then
        echo "ERROR: ${name} must be in range [${min},${max}]"
        errors=$((errors + 1))
    fi
}

echo "================================================="
echo "VBGW Production Env Validation"
echo "================================================="
echo "Env file: $ENV_FILE"

profile_override="${VALIDATE_PROFILE:-}"
profile="${profile_override:-${VBGW_PROFILE:-dev}}"
echo "Profile : $profile"
if [[ -n "$profile_override" ]]; then
    echo "Profile Override : VALIDATE_PROFILE=${profile_override}"
fi

strict_mode=false
if [[ "$profile" != "prod" && "$profile" != "production" ]]; then
    echo "WARN : VBGW_PROFILE is not production (current: $profile)"
    echo "      For production validation, set VBGW_PROFILE=production"
else
    strict_mode=true
fi

require_set SIP_PORT
require_set AI_ENGINE_ADDR
require_set HTTP_PORT
require_set MAX_CONCURRENT_CALLS
require_set ADMIN_API_KEY

if is_true "${REQUIRE_PRODUCTION_PROFILE:-0}" && [[ "$strict_mode" != "true" ]]; then
    echo "ERROR: REQUIRE_PRODUCTION_PROFILE=1 but profile is '${profile}'"
    errors=$((errors + 1))
fi

if [[ "$strict_mode" == "true" ]]; then
    if [[ "${ADMIN_API_KEY:-}" == "changeme-admin-key" ]] || \
        ! is_strong_admin_key "${ADMIN_API_KEY:-}"; then
        echo "ERROR: ADMIN_API_KEY must be strong (>=16 chars, upper/lower/digit/symbol, non-default)"
        errors=$((errors + 1))
    fi

    if ! is_true "${SIP_USE_TLS:-0}"; then
        echo "ERROR: SIP_USE_TLS must be enabled (1/true)"
        errors=$((errors + 1))
    fi
    if ! is_true "${GRPC_USE_TLS:-0}"; then
        echo "ERROR: GRPC_USE_TLS must be enabled (1/true)"
        errors=$((errors + 1))
    fi
    if ! is_true "${SRTP_ENABLE:-0}"; then
        echo "ERROR: SRTP_ENABLE must be enabled (1/true)"
        errors=$((errors + 1))
    fi
    if is_true "${PJSIP_NULL_AUDIO:-0}"; then
        echo "ERROR: PJSIP_NULL_AUDIO must be disabled (0/false) in production"
        errors=$((errors + 1))
    fi

    require_int_range ADMIN_API_RATE_LIMIT_RPS 1 10000
    require_int_range ADMIN_API_RATE_LIMIT_BURST 1 100000
    require_int_range ADMIN_API_MAX_BODY_BYTES 256 1048576
    require_int_range ADMIN_API_MAX_HEADER_BYTES 1024 262144

    if [[ -n "${ADMIN_API_RATE_LIMIT_RPS:-}" && -n "${ADMIN_API_RATE_LIMIT_BURST:-}" ]] &&
        [[ "${ADMIN_API_RATE_LIMIT_RPS}" =~ ^[0-9]+$ ]] &&
        [[ "${ADMIN_API_RATE_LIMIT_BURST}" =~ ^[0-9]+$ ]] &&
        (( ADMIN_API_RATE_LIMIT_BURST < ADMIN_API_RATE_LIMIT_RPS )); then
        echo "ERROR: ADMIN_API_RATE_LIMIT_BURST must be >= ADMIN_API_RATE_LIMIT_RPS"
        errors=$((errors + 1))
    fi

    if [[ "${SIP_PORT:-}" =~ ^[0-9]+$ ]] && [[ "${HTTP_PORT:-}" =~ ^[0-9]+$ ]] &&
        (( SIP_PORT == HTTP_PORT )); then
        echo "ERROR: SIP_PORT and HTTP_PORT must not be the same"
        errors=$((errors + 1))
    fi

    require_file SIP_TLS_CERT_FILE
    require_file SIP_TLS_PRIVKEY_FILE
    require_file SIP_TLS_CA_FILE
    require_file GRPC_TLS_CA_CERT
    require_file GRPC_TLS_CLIENT_CERT
    require_file GRPC_TLS_CLIENT_KEY
fi

if [[ "$errors" -gt 0 ]]; then
    echo ""
    echo "FAIL: ${errors} validation error(s) detected."
    exit 2
fi

echo ""
echo "PASS: production env validation succeeded."
