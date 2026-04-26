#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-6789}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_DIR="${REPO_ROOT}/miniob/db"

echo "[clean_db] repo_root=${REPO_ROOT}"
echo "[clean_db] port=${PORT}"
echo "[clean_db] db_dir=${DB_DIR}"

# Stop any listener on the port (best-effort)
if command -v lsof >/dev/null 2>&1; then
  pids="$(lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN -t 2>/dev/null || true)"
  if [[ -n "${pids}" ]]; then
    echo "[clean_db] killing listeners on port ${PORT}: ${pids}"
    # shellcheck disable=SC2086
    kill ${pids} 2>/dev/null || true
  fi
fi

# Also stop stray observer processes (best-effort)
if command -v pkill >/dev/null 2>&1; then
  pkill -f "${REPO_ROOT}/build/bin/observer" 2>/dev/null || true
  pkill -f "./build/bin/observer" 2>/dev/null || true
fi

rm -rf "${DB_DIR}"
mkdir -p "${DB_DIR}"

echo "[clean_db] done"

