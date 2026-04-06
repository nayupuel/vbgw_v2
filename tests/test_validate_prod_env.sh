#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VALIDATOR="$ROOT_DIR/scripts/validate_prod_env.sh"
TMP_DIR="$(mktemp -d /tmp/vbgw_validate_env.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -f "$VALIDATOR" ]]; then
    echo "FAIL: validator script not found: $VALIDATOR"
    exit 1
fi

echo "[1/3] profile enforcement check..."
cat >"$TMP_DIR/dev.env" <<'EOF'
VBGW_PROFILE=dev
SIP_PORT=5060
AI_ENGINE_ADDR=localhost:50051
HTTP_PORT=8080
MAX_CONCURRENT_CALLS=10
ADMIN_API_KEY=changeme-admin-key
EOF

set +e
REQUIRE_PRODUCTION_PROFILE=1 "$VALIDATOR" "$TMP_DIR/dev.env" >/tmp/vbgw_validate_dev.log 2>&1
dev_rc=$?
set -e
if [[ "$dev_rc" -eq 0 ]]; then
    echo "FAIL: dev profile should fail when REQUIRE_PRODUCTION_PROFILE=1"
    cat /tmp/vbgw_validate_dev.log
    exit 1
fi

echo "[2/3] production happy-path check..."
touch "$TMP_DIR/sip-cert.pem" "$TMP_DIR/sip-key.pem" "$TMP_DIR/sip-ca.pem"
touch "$TMP_DIR/ca.pem" "$TMP_DIR/client.pem" "$TMP_DIR/client-key.pem"
cat >"$TMP_DIR/prod_ok.env" <<EOF
VBGW_PROFILE=production
SIP_PORT=5060
AI_ENGINE_ADDR=localhost:50051
HTTP_PORT=8080
MAX_CONCURRENT_CALLS=100
ADMIN_API_KEY=Strong_Admin_Key_2026!
SIP_USE_TLS=1
GRPC_USE_TLS=1
SRTP_ENABLE=1
PJSIP_NULL_AUDIO=0
SIP_TLS_CERT_FILE=$TMP_DIR/sip-cert.pem
SIP_TLS_PRIVKEY_FILE=$TMP_DIR/sip-key.pem
SIP_TLS_CA_FILE=$TMP_DIR/sip-ca.pem
GRPC_TLS_CA_CERT=$TMP_DIR/ca.pem
GRPC_TLS_CLIENT_CERT=$TMP_DIR/client.pem
GRPC_TLS_CLIENT_KEY=$TMP_DIR/client-key.pem
ADMIN_API_RATE_LIMIT_RPS=20
ADMIN_API_RATE_LIMIT_BURST=40
ADMIN_API_MAX_BODY_BYTES=8192
ADMIN_API_MAX_HEADER_BYTES=16384
EOF

"$VALIDATOR" "$TMP_DIR/prod_ok.env" >/tmp/vbgw_validate_prod_ok.log 2>&1

echo "[3/3] production guardrail failure check..."
cat >"$TMP_DIR/prod_bad.env" <<EOF
VBGW_PROFILE=production
SIP_PORT=5060
AI_ENGINE_ADDR=localhost:50051
HTTP_PORT=5060
MAX_CONCURRENT_CALLS=100
ADMIN_API_KEY=weakkey
SIP_USE_TLS=1
GRPC_USE_TLS=1
SRTP_ENABLE=1
PJSIP_NULL_AUDIO=0
SIP_TLS_CERT_FILE=$TMP_DIR/sip-cert.pem
SIP_TLS_PRIVKEY_FILE=$TMP_DIR/sip-key.pem
SIP_TLS_CA_FILE=$TMP_DIR/sip-ca.pem
GRPC_TLS_CA_CERT=$TMP_DIR/ca.pem
GRPC_TLS_CLIENT_CERT=$TMP_DIR/client.pem
GRPC_TLS_CLIENT_KEY=$TMP_DIR/client-key.pem
ADMIN_API_RATE_LIMIT_RPS=50
ADMIN_API_RATE_LIMIT_BURST=20
ADMIN_API_MAX_BODY_BYTES=8192
ADMIN_API_MAX_HEADER_BYTES=16384
EOF

set +e
"$VALIDATOR" "$TMP_DIR/prod_bad.env" >/tmp/vbgw_validate_prod_bad.log 2>&1
bad_rc=$?
set -e
if [[ "$bad_rc" -eq 0 ]]; then
    echo "FAIL: invalid production env should fail validation"
    cat /tmp/vbgw_validate_prod_bad.log
    exit 1
fi

echo "PASS: validate_prod_env.sh checks are working as expected."
