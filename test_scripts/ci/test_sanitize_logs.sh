#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
# Copyright (c) 2025 Infernet Systems Pvt Ltd
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SANITIZER="${SCRIPT_DIR}/sanitize_logs.sh"

if [ ! -x "${SANITIZER}" ]; then
  echo "Error: Sanitizer script ${SANITIZER} not found or not executable"
  exit 1
fi

echo "Running log sanitization pattern verification tests..."

TEST_INPUT=$(cat <<'EOF'
[DEBUG] Authorization: Bearer dummy-bearer-token-12345
[DEBUG] Authorization: Basic dXNlcjpwYXNz
[DEBUG] Authorization: Basic unencoded_username:unencoded_password
[DEBUG] Header: X-API-KEY: secret-microservice-key
[DEBUG] Incoming payload: {"token": "jwt-token-secret-999", "password": "super-secret-password", "client_secret": "app-secret-key"}
[DEBUG] Python dict: {'token': 'single-quoted-token', 'password': 'single-quoted-password'}
[DEBUG] Simple colon: password: secret-pw
[DEBUG] Environment: STORAGE_TYPE_POSTGRESQL_PASSWORD=postgres-pw-secret
[DEBUG] Environment: RESTAPI_HOST_KEY_PASSWORD=ssl-key-pw-secret
[DEBUG] Environment: SERVICE_KEY_PASSWORD=service-pw-secret
[DEBUG] Property: storage.type.postgresql.password = db-pw-secret
[DEBUG] Request URI: /api/v1/validateSubToken?token=query-param-token-123&user=sub1
[DEBUG] Boundary check: monkey=banana turnkey=enabled hockey=ice
[DEBUG] ApiKey camelCase: apiKey: secret-apikey
[INFO] GET /api/v1/groups?serialNumber=112233445566 HTTP/1.1 200 OK
[DEBUG] Device MAC: 00:11:22:33:44:55, TransactionId: 10042
EOF
)

SANITIZED=$(echo "$TEST_INPUT" | "${SANITIZER}")

# Check 1: Ensure sensitive values are not present
LEAKED=0
for secret in \
  "dummy-bearer-token-12345" \
  "dXNlcjpwYXNz" \
  "unencoded_username:unencoded_password" \
  "secret-microservice-key" \
  "jwt-token-secret-999" \
  "super-secret-password" \
  "app-secret-key" \
  "single-quoted-token" \
  "single-quoted-password" \
  "secret-pw" \
  "postgres-pw-secret" \
  "ssl-key-pw-secret" \
  "service-pw-secret" \
  "db-pw-secret" \
  "query-param-token-123" \
  "secret-apikey"; do
  if echo "$SANITIZED" | grep -q "$secret"; then
    echo "FAIL: Sensitive secret leaked in sanitized output: $secret"
    LEAKED=1
  fi
done

if [ "$LEAKED" -ne 0 ]; then
  echo "Sanitized output:"
  echo "$SANITIZED"
  exit 1
fi

# Check 2: Ensure [REDACTED] replacement exists
REDACTED_COUNT=$(echo "$SANITIZED" | grep -o "\[REDACTED\]" | wc -l)
if [ "$REDACTED_COUNT" -lt 13 ]; then
  echo "FAIL: Expected at least 13 [REDACTED] replacements, found $REDACTED_COUNT"
  echo "Sanitized output:"
  echo "$SANITIZED"
  exit 1
fi

# Check 3: Ensure non-sensitive diagnostic logs are preserved
if ! echo "$SANITIZED" | grep -q 'GET /api/v1/groups?serialNumber=112233445566 HTTP/1.1 200 OK'; then
  echo "FAIL: Non-sensitive HTTP request line was altered"
  exit 1
fi

if ! echo "$SANITIZED" | grep -q 'Device MAC: 00:11:22:33:44:55, TransactionId: 10042'; then
  echo "FAIL: Non-sensitive diagnostic line was altered"
  exit 1
fi

# Check 4: Ensure boundary check values are not over-redacted
for safe_pattern in "monkey=banana" "turnkey=enabled" "hockey=ice" "&user=sub1"; do
  if ! echo "$SANITIZED" | grep -q "$safe_pattern"; then
    echo "FAIL: Non-sensitive boundary pattern was altered or over-redacted: $safe_pattern"
    exit 1
  fi
done

echo "Log pattern verification: PASS (${REDACTED_COUNT} patterns successfully redacted)"

# Check 5: Pipeline Simulation Test (Testing stream -> tail -> sanitize -> file)
echo "Running CI pipeline simulation test..."
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT

SYNTH_LOG="${TEMP_DIR}/synthetic_raw.log"
OUT_LOG="${TEMP_DIR}/userportal-debug.log"

for i in $(seq 1 600); do
  if [ "$i" -eq 50 ]; then
    echo "Line $i: Early dropped secret: Authorization: Bearer early-secret-token" >> "${SYNTH_LOG}"
  elif [ "$i" -eq 250 ]; then
    echo "Line $i: Mid-stream header: X-API-KEY: mid-stream-secret-key" >> "${SYNTH_LOG}"
  elif [ "$i" -eq 550 ]; then
    echo "Line $i: Late secret: {\"password\": \"late-secret-password\"}" >> "${SYNTH_LOG}"
  else
    echo "Line $i: Normal operational log event $i at 2026-09-25T16:00:00Z" >> "${SYNTH_LOG}"
  fi
done

# Execute exact pipeline used by CI failure handler
tail -500 "${SYNTH_LOG}" | "${SANITIZER}" > "${OUT_LOG}"

if [ ! -f "${OUT_LOG}" ]; then
  echo "FAIL: Output file ${OUT_LOG} was not created"
  exit 1
fi

OUT_LINES=$(wc -l < "${OUT_LOG}")
if [ "${OUT_LINES}" -ne 500 ]; then
  echo "FAIL: Expected exactly 500 lines in bounded log, found ${OUT_LINES}"
  exit 1
fi

# Assert content before the tail window (line 50) was properly dropped
if grep -q "early-secret-token" "${OUT_LOG}"; then
  echo "FAIL: Log before tail boundary was retained in output"
  exit 1
fi

# Assert retained secrets are redacted in output file
for sec in "mid-stream-secret-key" "late-secret-password"; do
  if grep -q "$sec" "${OUT_LOG}"; then
    echo "FAIL: Retained secret $sec was not redacted in simulated CI output file"
    exit 1
  fi
done

# Assert non-sensitive line from tail is intact
if ! grep -q "Normal operational log event 500" "${OUT_LOG}"; then
  echo "FAIL: Diagnostic log line 500 was missing from simulated output"
  exit 1
fi

echo "CI pipeline simulation: PASS (500 lines bounded, pre-boundary lines dropped, retained secrets redacted)"
