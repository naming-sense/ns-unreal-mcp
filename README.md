# ns-unreal-mcp

An all-in-one repository for the Unreal Editor MCP plugin and the Python MCP server bridge.

## Key Features

- Unreal Editor MCP toolset
  - Asset query/create/duplicate/rename/delete (`asset.*`)
  - Object/settings inspection and patching (`object.*`, `settings.*`)
  - UMG tree inspection, widget/slot patching, blueprint create/reparent (`umg.*`)
  - Material and Niagara parameter inspection/edit (`mat.*`, `niagara.*`)
- MCP server
  - Standard stdio MCP server + UE WebSocket bridge
  - Dynamic `tools.list` sync and pass-through calls
  - `event.*` streaming, metrics/logs, retries, WSL/Windows endpoint fallback
  - Multi-UE instance selector support

## Repository Layout

- `ue5-mcp-plugin/`: Unreal Engine plugin source (`UnrealMCP.uplugin`)
- `mcp_server/`: Python MCP server (stdio + UE WebSocket bridge)
- `mcp_server/docs/INTERNAL_ONEPAGE_SETUP.md`: one-page internal deployment guide

## Installation

This is a single repo, but installation paths are split:

1. Install plugin
   - `ue5-mcp-plugin/` → `<YourProject>/Plugins/ue5-mcp-plugin`
2. Install MCP server
   - Keep `mcp_server/` in a stable path (example: `D:\Tools\ns-unreal-mcp\mcp_server`)

## Local UE Project Sync

Use local Unreal projects (for example `TestMcp`) as runtime validation targets only.

- Linux/WSL:
  - `./scripts/sync_plugin_to_testmcp.sh <UE_PROJECT_PATH>`
- Windows PowerShell:
  - `.\scripts\sync_plugin_to_testmcp.ps1 -ProjectDir <UE_PROJECT_PATH>`

## MCP Client Registration

### 1) Codex CLI

- WSL/Linux:
  - `codex mcp add ue-mcp -- /path/to/mcp_server/scripts/run_mcp_server.sh`
- Windows:
  - `codex mcp add ue-mcp -- D:\\path\\to\\mcp_server\\scripts\\run_mcp_server.cmd`

### 2) Claude Code

- User-scope registration example:
  - `claude mcp add-json ue-mcp '{"type":"stdio","command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}' --scope user`
- Windows example:
  - `claude mcp add-json ue-mcp "{\"type\":\"stdio\",\"command\":\"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd\"}" --scope user`

### 3) Cursor

- Create `.cursor/mcp.json` in your project:
  - Linux/WSL:
    - `{"mcpServers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
  - Windows:
    - `{"mcpServers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`
- For global registration, you can use `~/.cursor/mcp.json`

### 4) GitHub Copilot (VS Code MCP)

- Create `.vscode/mcp.json` in your workspace:
  - Linux/WSL:
    - `{"servers":{"ue-mcp":{"command":"/path/to/mcp_server/scripts/run_mcp_server.sh"}}}`
  - Windows:
    - `{"servers":{"ue-mcp":{"command":"D:\\\\path\\\\to\\\\mcp_server\\\\scripts\\\\run_mcp_server.cmd"}}}`
- Ensure MCP is enabled in VS Code. You can also register via Command Palette: `MCP: Add Server`.

## References

- Runtime and operations: `mcp_server/docs/RUNBOOK.md`
- Troubleshooting: `mcp_server/docs/TROUBLESHOOTING.md`
- Multi-instance selector: `mcp_server/docs/MULTI_INSTANCE_SELECTOR.md`
- Single-repo workflow: `docs/SINGLE_REPO_WORKFLOW.md`
- Release notes: `RELEASE_NOTES_v0.1.3.md`
