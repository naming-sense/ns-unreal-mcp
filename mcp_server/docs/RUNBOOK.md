# UE MCP Server Runbook

## 1. 사전 조건
- Unreal Editor 5.7 실행 상태
- `TestMcp/Plugins/ue5-mcp-plugin` 플러그인 활성화
- Python 3.11 이상
- UE 플러그인 WS 채널 확인: 기본 `ws://127.0.0.1:19090`
- UE 플러그인 실행 시 `Saved/UnrealMCP/connection.json` 생성 확인(자동 endpoint 탐지용)

## 2. 로컬 실행(빠른 시작)
1. 의존성 설치
   - `cd mcp_server`
   - `pip install -e .[dev]`
2. 설정 파일 준비
   - `cp configs/config.example.yaml configs/config.yaml`
3. 헬스 체크
   - `python -m mcp_server --config configs/config.yaml --once-health`
4. 툴 카탈로그 확인
   - `python -m mcp_server --config configs/config.yaml --once-tools`
5. 서버 상시 실행(필요 시)
   - `python -m mcp_server --config configs/config.yaml`
6. stdio 런처 실행(권장)
   - `bash scripts/run_mcp_server.sh`

## 3. 샘플 Python 클라이언트 사용
- 도구 목록 조회:
  - `python examples/agent_tool_client.py --config configs/config.yaml --list-tools`
- 매터리얼 파라미터 조회:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool mat.instance.params.get --params-json '{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`
- 이벤트 스트리밍 포함 호출:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool system.health --stream-events`

## 3-1. Codex CLI MCP 등록(stdio)
- stdio MCP 서버 실행:
  - `bash scripts/run_mcp_server.sh`
- Codex CLI 등록:
  - `codex mcp add ue-mcp -- /mnt/d/codex-cli/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.sh`
- 등록 확인:
  - `codex mcp list`
  - `codex mcp get ue-mcp --json`
- 프레이밍 호환성:
  - `mcp_server.mcp_stdio`는 `Content-Length` framed JSON과 JSON line 입력을 모두 허용한다.
  - Codex 클라이언트 버전에 따라 startup `initialize`가 JSON line으로 들어올 수 있으므로, stdio 호환 모드를 유지한다.
- startup timeout 권장:
  - `~/.codex/config.toml`에 아래 값을 넣어 startup timeout을 완화한다.
  - `[mcp_servers.ue-mcp]`
  - `startup_timeout_sec = 30`
- endpoint 자동탐지 우선순위:
  - `UE_MCP_WS_URL` > `UE_MCP_CONNECTION_FILE` > `ue.connection_file`
  - `UE_MCP_PROJECT_ROOT`/`ue.project_root` 하위 `Saved/UnrealMCP/connection.json`
  - 마지막 fallback은 `ue.ws_url`

## 4. 출력 형식 요약
- 일반 호출: 단일 JSON 출력
- 스트리밍 호출:
  - `{"type":"event","event":...}` 다중 라인
  - 마지막에 `{"type":"result","result":...}` 1개 라인

## 5. 운영 모드 권장 옵션
- JSON 로그:
  - `server.json_logs: true`
- 재시도:
  - `retry.transient_max_attempts`, `retry.backoff_initial_s`, `retry.backoff_max_s`
- 메트릭:
  - `metrics.enabled: true`
  - `metrics.log_interval_s: 30`
  - one-shot 시 `--print-metrics`로 스냅샷 확인

## 6. 30분 온보딩 체크리스트
- `once-health`가 0 코드로 종료
- `once-tools`에서 `schema_hash`, `tools[]` 출력 확인
- `mat.instance.params.get` 호출 성공 확인
- `--stream-events` 호출 시 `event.*` + `result` 순으로 출력 확인
- `pytest` 통과 확인

## 7. E2E 스모크(mat/niagara/umg + asset lifecycle)
- 자동 탐색 기반 실행:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --stream-events`
- 지정 경로 기반 실행:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --material-path "/Game/Materials/MI_Test.MI_Test" --niagara-path "/Game/VFX/NS_Test.NS_Test" --umg-path "/Game/UI/WBP_Test.WBP_Test" --require-all`
- lifecycle 임시 경로/소스 지정:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-root-path "/Game/MCPRuntimeE2E" --asset-lifecycle-source "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"`
- lifecycle 제외 실행:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --skip-asset-lifecycle`
- lifecycle 에셋 유지(기본은 마지막 단계에서 삭제):
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-keep-assets`
- 실제 툴 결과까지 포함:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --stream-events --require-all --include-result`
- 결과 판독:
  - `ok=true`: 전체 성공
  - `scenarios[*].status=skipped`: 경로 미지정/자동탐색 실패(기본은 실패 아님)
  - `scenarios[*].scenario=asset_lifecycle`: `steps[*]`에 duplicate/rename/create/delete 단계별 결과 기록
  - `scenarios[*].summary.kept_assets=true`: delete 단계를 건너뛰고 생성 에셋을 유지한 실행
  - `--require-all` 사용 시 `skipped`도 실패 처리

## 8. E2E 모드 고정 검증(권장)
- cleanup + keep-assets를 연속 실행하고 모드별 기대값(`deleted_count`)까지 검증:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml`
- cleanup만 검증:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode cleanup`
- keep-assets만 검증:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode keep`
- CI 템플릿:
  - `.github/workflows/mcp-server-e2e-modes.yml` (`workflow_dispatch`, self-hosted linux)
