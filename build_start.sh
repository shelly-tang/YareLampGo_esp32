#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/scripts/flash.sh" "$@"
