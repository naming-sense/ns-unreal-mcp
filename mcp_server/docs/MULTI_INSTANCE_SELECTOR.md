# Multi-Instance Selector Quick Guide

다중 Unreal Editor 인스턴스에서 `ue-mcp`를 안정적으로 쓰기 위한 최소 가이드입니다.

## 1) 후보 확인
```bash
python -m mcp_server.mcp_stdio --config configs/config.yaml --once-endpoints
```

- 출력 `candidates[]`에서 대상 인스턴스를 고릅니다.
- `selector_hint.env`는 환경변수 방식, `selector_hint.args`는 CLI 인자 방식입니다.

## 2) Codex 등록(WSL/Linux)
```bash
UE_MCP_INSTANCE_ID="<instance_id>" codex mcp add ue-mcp -- /path/to/mcp_server/scripts/run_mcp_server.sh
```

## 3) Codex 등록(Windows)
```powershell
$env:UE_MCP_INSTANCE_ID = "<instance_id>"
codex mcp add ue-mcp -- D:\path\to\mcp_server\scripts\run_mcp_server.cmd
```

## 4) VSCode Copilot MCP 등록 예시
```json
{
  "mcp.servers": {
    "ue-mcp": {
      "type": "stdio",
      "command": "/path/to/mcp_server/scripts/run_mcp_server.sh",
      "env": {
        "UE_MCP_INSTANCE_ID": "<instance_id>"
      }
    }
  }
}
```

## 5) 실패 시 우선 확인
- selector 미지정 상태로 UE 인스턴스가 2개 이상인지
- 선택한 `instance_id`가 `--once-endpoints` 출력에 실제로 존재하는지
- UE 로그에 `Started MCP WS event transport ...`가 유지되는지
