# ns-unreal-mcp v0.1.3

Release date: 2026-02-26

## 요약

v0.1.3은 다중 Unreal 인스턴스 환경 안정화와 MCP 클라이언트 호환성을 강화한 릴리즈입니다.  
`stdio` MCP 서버 경로를 중심으로 Codex/Claude/Cursor/Copilot 연동 구성을 정리했고, UE 플러그인 쪽 UMG/툴 레지스트리 업데이트를 포함합니다.

## 주요 변경점

### Added
- 다중 UE 인스턴스 selector 문서 추가: `mcp_server/docs/MULTI_INSTANCE_SELECTOR.md`
- 배포 보조 스크립트 및 압축 산출물 추가: `scripts/`, `release-assets/`
- MCP 런타임/설정/엔드포인트 관련 단위 테스트 보강

### Changed
- MCP 서버:
  - endpoint 탐색/선택 로직 강화 (`ws_endpoint.py`, `config.py`, `app.py`, `mcp_stdio.py`)
  - tool pass-through/transport 신뢰성 개선 (`tool_passthrough.py`, `ue_transport.py`, `mcp_facade.py`)
  - 실행/장애 대응 문서 업데이트 (`RUNBOOK`, `TROUBLESHOOTING`, `INTERNAL_ONEPAGE_SETUP`)
- UE 플러그인:
  - WebSocket transport 및 command router 개선
  - Tool registry/스키마 갱신 (`schemas_30_tools.json`)
  - 런타임 자동화 테스트 갱신

### Fixed
- WSL/Windows 혼합 환경에서 UE endpoint 선택 실패/연결 불안정 문제 완화
- stdio MCP startup 시 일부 클라이언트 초기화 패턴 호환성 개선

## 호환성

- Unreal Plugin: UE 5.x 소스 빌드 환경 기준
- MCP Server: Python 3.10+ 권장
- MCP Clients: Codex CLI, Claude Code, Cursor, GitHub Copilot(VS Code MCP)

## 업그레이드 가이드

1. Unreal 프로젝트의 기존 `Plugins/ue5-mcp-plugin`을 v0.1.3 소스로 교체
2. `mcp_server` 디렉터리를 새 버전으로 교체 후 재설치
   - `pip install -e .[dev]`
3. 각 MCP 클라이언트의 서버 등록 경로가 최신 launcher 스크립트를 가리키는지 확인
4. UE Editor 재시작 후 `tools.list` / `system.health`로 연결 검증

