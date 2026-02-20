#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/.." && pwd)"

CONFIG_PATH="${UE_MCP_CONFIG:-${ROOT_DIR}/configs/config.yaml}"
if [[ ! -f "${CONFIG_PATH}" && -f "${ROOT_DIR}/configs/config.example.yaml" ]]; then
  CONFIG_PATH="${ROOT_DIR}/configs/config.example.yaml"
fi

if [[ -z "${UE_MCP_CONNECTION_FILE:-}" ]]; then
  if [[ -n "${UE_MCP_PROJECT_ROOT:-}" ]]; then
    export UE_MCP_CONNECTION_FILE="${UE_MCP_PROJECT_ROOT}/Saved/UnrealMCP/connection.json"
  else
    FOUND_UPROJECT="$(find "${REPO_ROOT}" -maxdepth 4 -type f -name '*.uproject' 2>/dev/null | head -n 1 || true)"
    if [[ -n "${FOUND_UPROJECT}" ]]; then
      export UE_MCP_PROJECT_ROOT="$(dirname "${FOUND_UPROJECT}")"
      export UE_MCP_CONNECTION_FILE="${UE_MCP_PROJECT_ROOT}/Saved/UnrealMCP/connection.json"
    fi
  fi
fi

PYTHON_BIN="${UE_MCP_PYTHON:-}"
if [[ -z "${PYTHON_BIN}" ]]; then
  if [[ -x "${ROOT_DIR}/.venv/bin/python" ]]; then
    PYTHON_BIN="${ROOT_DIR}/.venv/bin/python"
  elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
  elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python)"
  else
    echo "Python executable not found. Set UE_MCP_PYTHON." >&2
    exit 1
  fi
fi

export PYTHONPATH="${ROOT_DIR}/src${PYTHONPATH:+:${PYTHONPATH}}"

exec "${PYTHON_BIN}" -m mcp_server.mcp_stdio --config "${CONFIG_PATH}" "$@"
