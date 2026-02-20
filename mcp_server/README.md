# UE MCP Server (Python)

UnrealMCP UE 5.7 플러그인과 AI Agent를 연결하는 MCP 서버 구현 디렉터리입니다.

## 현재 범위
- M0: 프로젝트 부트스트랩
- M1: UE WebSocket 브리지(`UeWsTransport`, `RequestBroker`)
- M2: 동적 툴 카탈로그 + pass-through(`MCPPassThroughService`)
- M3: 요청 단위 이벤트 스트리밍(`EventRouter`, `--stream-events`)
- M4: 신뢰성/운영성(`retry`, JSON 로그, 런타임 metrics, fallback 에러 페이로드)
- M5: 실행 가이드/트러블슈팅/릴리스 체크리스트 + 샘플 Python 클라이언트

## 빠른 시작
1. 의존성 설치
   - `pip install -e .[dev]`
2. 예시 설정 복사
   - `cp configs/config.example.yaml configs/config.yaml`
3. 서버 실행
   - `python -m mcp_server --config configs/config.yaml`
4. stdio 서버 런처(권장)
   - `bash scripts/run_mcp_server.sh`

## 유틸 실행 모드
- 헬스 체크 1회:
  - `python -m mcp_server --once-health`
- 툴 카탈로그 동기화/출력:
  - `python -m mcp_server --once-tools`
- 특정 툴 1회 호출(pass-through):
  - `python -m mcp_server --call-tool mat.instance.params.get --params-json '{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`
- 특정 툴 호출 중 `event.*` 실시간 출력:
  - `python -m mcp_server --call-tool system.health --stream-events`
- one-shot 실행 종료 시 런타임 metrics 출력:
  - `python -m mcp_server --once-tools --print-metrics`

## 샘플 Python 클라이언트
- 도구 목록 확인:
  - `python examples/agent_tool_client.py --config configs/config.yaml --list-tools`
- 도구 호출:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool mat.instance.params.get --params-json '{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`
- 이벤트 스트리밍 호출:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool system.health --stream-events`

## E2E 스모크 러너(mat/niagara/umg + asset lifecycle)
- 자동 asset.find 탐색 + 4개 시나리오 실행(`material`, `niagara`, `umg`, `asset_lifecycle`):
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --stream-events`
- 경로 직접 지정 + 전체 필수 검증:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --material-path "/Game/Materials/MI_Test.MI_Test" --niagara-path "/Game/VFX/NS_Test.NS_Test" --umg-path "/Game/UI/WBP_Test.WBP_Test" --require-all`
- asset lifecycle 임시 루트/소스 변경:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-root-path "/Game/MCPRuntimeE2E" --asset-lifecycle-source "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"`
- asset lifecycle 시나리오 제외:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --skip-asset-lifecycle`
- asset lifecycle 결과 에셋 유지(기본은 정리/삭제):
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-keep-assets`
- 출력은 단일 JSON 요약이며 `scenarios[*]`에 `status/duration_ms/event_count/summary`가 기록됩니다.
- 실제 툴 결과(JSON payload)까지 보고 싶으면 `--include-result`를 추가합니다(출력이 커질 수 있음).

## E2E 모드 고정 실행(cleanup + keep-assets)
- 두 모드를 순차 실행하고 기대값까지 검증:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml`
- cleanup만 실행:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode cleanup`
- keep-assets만 실행:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode keep`
- 추가 인자를 e2e 러너로 전달:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --extra-arg=--include-result`

## 운영 설정 포인트
- `server.json_logs`: JSON 구조 로그 활성화
- `retry.*`: transient 재시도 횟수/백오프 설정
- `metrics.*`: 런타임 메트릭 활성화/주기 로그 간격 설정
- `ue.connection_file`: UE가 기록한 `Saved/UnrealMCP/connection.json` 경로를 직접 지정
- `ue.project_root`: `<project_root>/Saved/UnrealMCP/connection.json` 자동 추론용 루트

## UE endpoint 자동탐지 우선순위
- `UE_MCP_WS_URL` 환경변수
- `UE_MCP_CONNECTION_FILE` 환경변수
- `ue.connection_file` 설정
- `UE_MCP_PROJECT_ROOT` / `ue.project_root` 하위 `Saved/UnrealMCP/connection.json`
- `ue.ws_url` 설정값
- UE 플러그인은 WS 서버 시작 시 `Saved/UnrealMCP/connection.json`을 자동으로 기록합니다.

## 메트릭 스냅샷 주요 필드
- `counters.*`: 연결/요청/이벤트/재시도 등 누적 카운터
- `gauges.*`: 현재 연결 상태, pending 요청 수, 구독 수 등 게이지
- `tool_metrics[*].p95_duration_ms`: 툴별 최근 샘플 기준 p95 지연시간
- `tool_metrics[*].failure_rate`: `(error_count + exception_count) / total_requests`

## 문서
- 사내용 1페이지 설치 가이드: `docs/INTERNAL_ONEPAGE_SETUP.md`
- 실행 가이드: `docs/RUNBOOK.md`
- 트러블슈팅: `docs/TROUBLESHOOTING.md`
- 릴리스 체크리스트: `docs/RELEASE_CHECKLIST.md`

## Codex CLI MCP 등록(stdio)
- 표준 MCP stdio 서버 실행:
  - `bash scripts/run_mcp_server.sh`
  - (Windows PowerShell) `powershell -ExecutionPolicy Bypass -File scripts/run_mcp_server.ps1`
  - (Windows CMD) `scripts\\run_mcp_server.cmd`
- Codex CLI에 서버 등록:
  - `codex mcp add ue-mcp -- /mnt/d/codex-cli/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.sh`
  - (Windows Codex) `codex mcp add ue-mcp -- D:\\path\\to\\ue5-mcp-plugin\\mcp_server\\scripts\\run_mcp_server.cmd`
- 등록 확인:
  - `codex mcp list`
  - `codex mcp get ue-mcp --json`
- 호환성 참고:
  - `mcp_server.mcp_stdio`는 `Content-Length` framed JSON과 JSON line 입력을 모두 지원합니다.
  - 일부 Codex 버전은 startup 단계에서 JSON line `initialize`를 전송하므로, 이 호환 모드가 필요합니다.
- startup timeout 권장:
  - UE 초기 연결/카탈로그 동기화 지연 시를 대비해 `~/.codex/config.toml`에 `startup_timeout_sec = 30` 이상 설정을 권장합니다.
  - 예시:
    - `[mcp_servers.ue-mcp]`
    - `startup_timeout_sec = 30`

## 테스트
- `pytest`
