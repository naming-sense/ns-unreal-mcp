# Single-Repository Workflow

This repository (`ns-unreal-mcp`) is the single source of truth.

## Scope

- Tracked in Git:
  - `mcp_server/`
  - `ue5-mcp-plugin/`
- Not tracked in Git:
  - Local Unreal validation projects (for example `TestMcp/`)

## Daily Development Rules

1. Edit code only inside this repository.
2. Do not edit plugin code directly inside a local Unreal project.
3. Before Unreal build/test, sync plugin from this repository to your local project:
   - Linux/WSL: `./scripts/sync_plugin_to_testmcp.sh <UE_PROJECT_PATH>`
   - Windows: `.\scripts\sync_plugin_to_testmcp.ps1 -ProjectDir <UE_PROJECT_PATH>`
4. Commit/push only from this repository.

## Migration Checklist (Completed)

- Backup branch created: `backup/pre-monorepo-switch-c1a2b1c`
- Backup tag created: `backup/pre-monorepo-switch-20260301`
- `.gitignore` updated for local test projects and Python local artifacts
- Sync scripts added for local Unreal project plugin deployment

## Recommended Validation Flow

1. `git status`
2. Run plugin sync script to local UE project.
3. Build Unreal project (VS/Build.bat/Live Coding).
4. Run MCP server smoke/E2E checks.
5. Commit and push from this repository.
