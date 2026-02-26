# ns-unreal-mcp

Unreal Editor용 MCP 플러그인 + Python MCP 서버(bridge) 통합 저장소입니다.

## 주요 기능

- Unreal Editor MCP 툴셋
  - 에셋 조회/생성/복제/리네임/삭제 (`asset.*`)
  - 객체/설정 인스펙션·패치 (`object.*`, `settings.*`)
  - UMG 트리 조회/위젯·슬롯 패치/블루프린트 생성·재부모화 (`umg.*`)
  - 머티리얼/나이아가라 파라미터 조회·수정 (`mat.*`, `niagara.*`)
- MCP 서버
  - stdio 표준 MCP 서버 + UE WebSocket 브리지
  - 동적 `tools.list` 동기화 및 pass-through 호출
  - `event.*` 스트리밍, 메트릭/로그, 재시도, WSL/Windows endpoint fallback
  - 다중 UE 인스턴스 선택(selector) 지원

## Repository Layout

- `ue5-mcp-plugin/`: Unreal Engine plugin source (`UnrealMCP.uplugin`)
- `mcp_server/`: Python MCP server (stdio + UE WebSocket bridge)
- `mcp_server/docs/INTERNAL_ONEPAGE_SETUP.md`: 사내 배포용 1페이지 설치 가이드

## 설치

단일 레포이지만 설치 위치는 분리합니다.

1. 플러그인 설치
   - `ue5-mcp-plugin/` → `<YourProject>/Plugins/ue5-mcp-plugin`
2. MCP 서버 설치
   - `mcp_server/`를 고정 경로에 보관 (예: `D:\Tools\ns-unreal-mcp\mcp_server`)

## MCP 클라이언트 등록

### 1) Codex CLI

- WSL/Linux:
  - `codex mcp add ue-mcp -- /path/to/mcp_server/scripts/run_mcp_server.sh`
- Windows:
  - `codex mcp add ue-mcp -- D:\\path\\to\\mcp_server\\scripts\\run_mcp_server.cmd`

### 2) Claude Code

- User scope 등록 예시:
  - `claude mcp add-json ue-mcp '{"type":"stdio","command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}' --scope user`
- Windows 예시:
  - `claude mcp add-json ue-mcp "{\"type\":\"stdio\",\"command\":\"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd\"}" --scope user`

### 3) Cursor

- 프로젝트 기준 `.cursor/mcp.json` 생성:
  - Linux/WSL:
    - `{"mcpServers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
  - Windows:
    - `{"mcpServers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`
- 전역 등록을 원하면 `~/.cursor/mcp.json` 사용 가능

### 4) GitHub Copilot (VS Code MCP)

- 워크스페이스 기준 `.vscode/mcp.json` 생성:
  - Linux/WSL:
    - `{"servers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
  - Windows:
    - `{"servers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`
- VS Code에서 MCP 기능이 켜져 있어야 하며, Command Palette의 `MCP: Add Server`로도 등록 가능

## 참고 문서

- 서버 실행/운영: `mcp_server/docs/RUNBOOK.md`
- 트러블슈팅: `mcp_server/docs/TROUBLESHOOTING.md`
- 멀티 인스턴스 선택: `mcp_server/docs/MULTI_INSTANCE_SELECTOR.md`
- 릴리즈 노트: `RELEASE_NOTES_v0.1.3.md`
