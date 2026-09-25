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

echo "Running log sanitization verification tests..."

TEST_INPUT=$(cat <<'EOF'
[DEBUG] Authorization: Bearer dummy-bearer-token-12345
[DEBUG] Incoming payload: {"token": "jwt-token-secret-999", "password": "super-secret-password", "client_secret": "app-secret-key"}
[DEBUG] Environment: STORAGE_TYPE_POSTGRESQL_PASSWORD=postgres-pw-secret
[DEBUG] Environment: RESTAPI_HOST_KEY_PASSWORD=ssl-key-pw-secret
[DEBUG] Environment: SERVICE_KEY_PASSWORD=service-pw-secret
[DEBUG] Property: storage.type.postgresql.password = db-pw-secret
[INFO] GET /api/v1/groups?serialNumber=112233445566 HTTP/1.1 200 OK
[DEBUG] Device MAC: 00:11:22:33:44:55, TransactionId: 10042
EOF
)

SANITIZED=$(echo "$TEST_INPUT" | "${SANITIZER}")

# Check 1: Ensure sensitive values are not present
LEAKED=0
for secret in \
  "dummy-bearer-token-12345" \
  "jwt-token-secret-999" \
  "super-secret-password" \
  "app-secret-key" \
  "postgres-pw-secret" \
  "ssl-key-pw-secret" \
  "service-pw-secret" \
  "db-pw-secret"; do
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
if [ "$REDACTED_COUNT" -lt 6 ]; then
  echo "FAIL: Expected at least 6 [REDACTED] replacements, found $REDACTED_COUNT"
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

echo "Log sanitization verification: PASS (${REDACTED_COUNT} patterns successfully redacted)"
