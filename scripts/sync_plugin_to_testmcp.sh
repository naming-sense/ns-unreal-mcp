#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_PLUGIN_DIR="${REPO_ROOT}/ue5-mcp-plugin"

TARGET_PROJECT_DIR="${1:-${UE_TESTMCP_PROJECT_DIR:-}}"
if [[ -z "${TARGET_PROJECT_DIR}" ]]; then
  echo "사용법: $0 <언리얼_프로젝트_경로>"
  echo "또는 환경변수 UE_TESTMCP_PROJECT_DIR 설정 후 $0"
  exit 1
fi

TARGET_PROJECT_DIR="$(cd "${TARGET_PROJECT_DIR}" && pwd)"
TARGET_PLUGIN_DIR="${TARGET_PROJECT_DIR}/Plugins/ue5-mcp-plugin"

if [[ ! -d "${SOURCE_PLUGIN_DIR}" ]]; then
  echo "소스 플러그인 경로를 찾을 수 없습니다: ${SOURCE_PLUGIN_DIR}"
  exit 1
fi

if [[ ! -d "${TARGET_PROJECT_DIR}" ]]; then
  echo "대상 프로젝트 경로를 찾을 수 없습니다: ${TARGET_PROJECT_DIR}"
  exit 1
fi

mkdir -p "${TARGET_PROJECT_DIR}/Plugins"

rsync -a --delete \
  --exclude '.git/' \
  --exclude 'Binaries/' \
  --exclude 'Intermediate/' \
  --exclude 'DerivedDataCache/' \
  --exclude 'Saved/' \
  "${SOURCE_PLUGIN_DIR}/" "${TARGET_PLUGIN_DIR}/"

echo "[완료] 플러그인 동기화:"
echo "  from: ${SOURCE_PLUGIN_DIR}"
echo "  to  : ${TARGET_PLUGIN_DIR}"
echo "다음 단계: UE 에디터에서 프로젝트를 열고 C++ 빌드를 수행하세요."
