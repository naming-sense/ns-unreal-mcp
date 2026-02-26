# UE MCP Server (Python)

This directory contains the Python MCP server implementation that bridges AI agents to the UnrealMCP UE plugin.

## Scope
- M0: project bootstrap
- M1: UE WebSocket bridge (`UeWsTransport`, `RequestBroker`)
- M2: dynamic tool catalog + pass-through (`MCPPassThroughService`)
- M3: request-scoped event streaming (`EventRouter`, `--stream-events`)
- M4: reliability and observability (`retry`, JSON logs, runtime metrics, fallback error payloads)
- M5: runbook/troubleshooting/release checklist + sample Python client

## Quick Start
1. Install dependencies
   - `pip install -e .[dev]`
2. Copy example config
   - `cp configs/config.example.yaml configs/config.yaml`
3. Run server
   - `python -m mcp_server --config configs/config.yaml`
4. Run stdio launcher (recommended)
   - `bash scripts/run_mcp_server.sh`

## Utility Modes
- One-shot health check:
  - `python -m mcp_server --once-health`
- Sync/print tool catalog:
  - `python -m mcp_server --once-tools`
- One-shot pass-through tool call:
  - `python -m mcp_server --call-tool mat.instance.params.get --params-json '{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`
- Stream `event.*` while calling a tool:
  - `python -m mcp_server --call-tool system.health --stream-events`
- Print runtime metrics at the end of a one-shot run:
  - `python -m mcp_server --once-tools --print-metrics`

## Sample Python Client
- List tools:
  - `python examples/agent_tool_client.py --config configs/config.yaml --list-tools`
- Call a tool:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool mat.instance.params.get --params-json '{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`
- Stream events for a tool call:
  - `python examples/agent_tool_client.py --config configs/config.yaml --tool system.health --stream-events`

## E2E Smoke Runner (mat/niagara/umg + asset lifecycle)
- Auto-discover with `asset.find` and run four scenarios (`material`, `niagara`, `umg`, `asset_lifecycle`):
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --stream-events`
- Specify paths and require all checks:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --material-path "/Game/Materials/MI_Test.MI_Test" --niagara-path "/Game/VFX/NS_Test.NS_Test" --umg-path "/Game/UI/WBP_Test.WBP_Test" --require-all`
- Override temporary lifecycle root/source:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-root-path "/Game/MCPRuntimeE2E" --asset-lifecycle-source "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"`
- Skip lifecycle scenario:
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --skip-asset-lifecycle`
- Keep lifecycle assets (default behavior is cleanup):
  - `python examples/e2e_smoke_runner.py --config configs/config.yaml --asset-lifecycle-keep-assets`
- Output format is a single JSON summary with `status/duration_ms/event_count/summary` per `scenarios[*]`.
- Add `--include-result` to include full tool payloads (can be large).

## Fixed E2E Mode Runner (cleanup + keep-assets)
- Run both modes sequentially with assertions:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml`
- Run cleanup mode only:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode cleanup`
- Run keep-assets mode only:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --mode keep`
- Forward extra args to e2e runner:
  - `python scripts/e2e_modes_runner.py --config configs/config.yaml --extra-arg=--include-result`

## Operational Config Highlights
- `server.json_logs`: enable structured JSON logs
- `retry.*`: transient retry count and backoff
- `metrics.*`: runtime metrics toggle and log interval
- `ue.connection_file`: explicit path to UE-written `Saved/UnrealMCP/connection.json`
- `ue.project_root`: root used to resolve `<project_root>/Saved/UnrealMCP/connection.json`

## UE Endpoint Discovery Priority
- `UE_MCP_WS_URL` environment variable
- `UE_MCP_CONNECTION_FILE` environment variable
- `ue.connection_file` in config
- `UE_MCP_PROJECT_ROOT` / `ue.project_root` + `Saved/UnrealMCP/connection.json`
- `ue.ws_url` in config
- The UE plugin automatically writes `Saved/UnrealMCP/connection.json` when WS starts.

## Key Metric Snapshot Fields
- `counters.*`: cumulative counters for connect/request/event/retry
- `gauges.*`: current connection state, pending requests, subscription count
- `tool_metrics[*].p95_duration_ms`: per-tool p95 latency over recent samples
- `tool_metrics[*].failure_rate`: `(error_count + exception_count) / total_requests`

## Documentation
- Internal one-page setup: `docs/INTERNAL_ONEPAGE_SETUP.md`
- Runtime guide: `docs/RUNBOOK.md`
- Troubleshooting: `docs/TROUBLESHOOTING.md`
- Release checklist: `docs/RELEASE_CHECKLIST.md`

## MCP Client Registration (stdio)
- Server launch entries:
  - Linux/WSL: `bash scripts/run_mcp_server.sh`
  - Windows PowerShell: `powershell -ExecutionPolicy Bypass -File scripts/run_mcp_server.ps1`
  - Windows CMD: `scripts\\run_mcp_server.cmd`

### Codex CLI
- `codex mcp add ue-mcp -- /path/to/mcp_server/scripts/run_mcp_server.sh`
- Windows: `codex mcp add ue-mcp -- D:\\path\\to\\mcp_server\\scripts\\run_mcp_server.cmd`
- Verify:
  - `codex mcp list`
- Recommended startup timeout:
  - `~/.codex/config.toml`
  - `[mcp_servers.ue-mcp]`
  - `startup_timeout_sec = 30`

### Claude Code
- `claude mcp add-json ue-mcp '{"type":"stdio","command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}' --scope user`
- Windows:
  - `claude mcp add-json ue-mcp "{\"type\":\"stdio\",\"command\":\"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd\"}" --scope user`

### Cursor
- `.cursor/mcp.json`:
  - `{"mcpServers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
- Windows:
  - `{"mcpServers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`

### GitHub Copilot (VS Code MCP)
- `.vscode/mcp.json`:
  - `{"servers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
- Windows:
  - `{"servers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`
- You can also add it from VS Code Command Palette: `MCP: Add Server`.

### Compatibility Notes
- `mcp_server.mcp_stdio` supports both `Content-Length` framed JSON and JSON-line input.
- Some clients send JSON-line `initialize` during startup, so compatibility mode is required.

## Tests
- `pytest`
