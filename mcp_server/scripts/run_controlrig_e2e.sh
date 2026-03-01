#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -x "$PROJECT_ROOT/.venv/bin/python" ]]; then
  PYTHON_EXEC="$PROJECT_ROOT/.venv/bin/python"
else
  PYTHON_EXEC="${PYTHON_EXEC:-python3}"
fi

cd "$PROJECT_ROOT"
exec "$PYTHON_EXEC" "$SCRIPT_DIR/controlrig_e2e_runner.py" --python-executable "$PYTHON_EXEC" "$@"
