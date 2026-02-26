# ns-unreal-mcp v0.1.3

Release date: 2026-02-26

## Summary

v0.1.3 focuses on stabilizing multi-Unreal-instance workflows and improving MCP client compatibility.  
It consolidates stdio MCP integration guidance for Codex/Claude/Cursor/Copilot and includes UMG/tool-registry updates on the UE plugin side.

## Highlights

### Added
- Added multi-UE instance selector documentation: `mcp_server/docs/MULTI_INSTANCE_SELECTOR.md`
- Added release helper scripts and packaged artifacts: `scripts/`, `release-assets/`
- Expanded unit tests for MCP runtime/config/endpoint behavior

### Changed
- MCP server:
  - Improved endpoint discovery/selection logic (`ws_endpoint.py`, `config.py`, `app.py`, `mcp_stdio.py`)
  - Improved tool pass-through/transport reliability (`tool_passthrough.py`, `ue_transport.py`, `mcp_facade.py`)
  - Updated operational docs (`RUNBOOK`, `TROUBLESHOOTING`, `INTERNAL_ONEPAGE_SETUP`)
- UE plugin:
  - Improved WebSocket transport and command router
  - Updated tool registry/schema (`schemas_30_tools.json`)
  - Updated runtime automation tests

### Fixed
- Reduced UE endpoint selection failures and connection instability in mixed WSL/Windows setups
- Improved startup compatibility for clients with different stdio MCP initialization patterns

## Compatibility

- Unreal Plugin: UE 5.x source-build environments
- MCP Server: Python 3.10+ recommended
- MCP Clients: Codex CLI, Claude Code, Cursor, GitHub Copilot(VS Code MCP)

## Upgrade Guide

1. Replace your existing Unreal project plugin at `Plugins/ue5-mcp-plugin` with v0.1.3 sources
2. Replace the `mcp_server` directory with v0.1.3 and reinstall
   - `pip install -e .[dev]`
3. Verify each MCP client points to the latest launcher script path
4. Restart Unreal Editor, then validate with `tools.list` and `system.health`
