# ns-unreal-mcp

A complete MCP server and Unreal Engine plugin set designed to streamline Unreal development workflows for AI agents.

## Repository layout

- `ue5-mcp-plugin/`  
  Unreal Engine plugin source (`UnrealMCP.uplugin`)
- `mcp_server/`  
  Python MCP server (stdio + UE WebSocket bridge)

## Installation (end users)

Use two installation paths even though this is a single repository:

1. Install plugin into your Unreal project:
   - Copy `ue5-mcp-plugin/` to:
   - `<YourProject>/Plugins/ue5-mcp-plugin`
2. Install MCP server to a tools directory:
   - Keep `mcp_server/` in a stable path (example: `D:\Tools\ue-mcp-server`)

Detailed one-page guide:
- `mcp_server/docs/INTERNAL_ONEPAGE_SETUP.md`

## Codex MCP registration

- WSL/Linux:
  - `codex mcp add ue-mcp -- /path/to/mcp_server/scripts/run_mcp_server.sh`
- Windows:
  - `codex mcp add ue-mcp -- D:\\path\\to\\mcp_server\\scripts\\run_mcp_server.cmd`
