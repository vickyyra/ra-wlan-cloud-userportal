#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0 OR LicenseRef-Commercial
# Copyright (c) 2025 Infernet Systems Pvt Ltd
set -euo pipefail

# Sanitizes sensitive patterns from log streams or files.
# Reads from file argument if provided, otherwise standard input.
if [ $# -ge 1 ] && [ "$1" != "-" ]; then
  INPUT="$1"
else
  INPUT="/dev/stdin"
fi

sed -E \
  -e 's/((Bearer|Basic)[[:space:]]+)[^"'\''\r\n]+/\1[REDACTED]/gI' \
  -e 's/([?&](token|password|secret|key|apiKey)=)[^&[:space:]"'\''`]+/\1[REDACTED]/gI' \
  -e 's/((^|[^a-zA-Z0-9])["'\''"]([a-zA-Z0-9_.-]*[._-])?(password|secret|token|key|apiKey)["'\''"][[:space:]]*:[[:space:]]*["'\''"])[^"'\''"]*(["'\''"])/\1[REDACTED]\5/gI' \
  -e 's/((^|[^a-zA-Z0-9])([a-zA-Z0-9_.-]*[._-])?(password|secret|token|key|apiKey)[[:space:]]*[:=][[:space:]]*)[^"'\''[:space:],;&]+/\1[REDACTED]/gI' \
  "$INPUT"
