# UE MCP 사내 배포용 1페이지 설치 가이드

이 문서는 **사내 사용자용 최소 절차**만 담은 빠른 설치 가이드입니다.  
(상세 운영/트러블슈팅은 `docs/RUNBOOK.md`, `docs/TROUBLESHOOTING.md` 참고)

---

## 0) 준비물
- Unreal 프로젝트에 플러그인 폴더 배치
  - `<Project>/Plugins/ue5-mcp-plugin`
- Python 3.11+
- Codex CLI 설치

---

## 1) 플러그인 설치 (공통)
1. 배포받은 `ue5-mcp-plugin` 폴더를 프로젝트 `Plugins/` 아래에 복사
2. Unreal Editor 실행 후 플러그인 로드 확인
3. 로그에서 WS 시작 확인
   - 예: `LogUnrealMCP: Started MCP WS event transport at ws://0.0.0.0:19090`
4. 아래 파일 생성 확인
   - `<Project>/Saved/UnrealMCP/connection.json`

> `connection.json`을 서버가 읽기 때문에, 사용자별 IP를 직접 입력할 필요가 없습니다.
> 다중 UE 인스턴스를 동시에 쓰면 `Saved/UnrealMCP/instances/*.json`도 함께 생성됩니다.

---

## 2) Python MCP 서버 설치

### A. Windows (PowerShell)
```powershell
cd D:\path\to\ue5-mcp-plugin\mcp_server
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -e .[dev]
Copy-Item .\configs\config.example.yaml .\configs\config.yaml -Force
```

### B. WSL/Linux (bash)
```bash
cd /path/to/ue5-mcp-plugin/mcp_server
python3 -m venv .venv
source .venv/bin/activate
pip install -e .[dev]
cp configs/config.example.yaml configs/config.yaml
```

---

## 3) 서버 수동 실행 확인

### Windows
```powershell
cd D:\path\to\ue5-mcp-plugin\mcp_server
powershell -ExecutionPolicy Bypass -File .\scripts\run_mcp_server.ps1
```

### WSL/Linux
```bash
cd /path/to/ue5-mcp-plugin/mcp_server
bash scripts/run_mcp_server.sh
```

---

## 4) Codex에 MCP 서버 등록

## 4-1) Windows Codex
```powershell
codex mcp remove ue-mcp
codex mcp add ue-mcp -- D:\path\to\ue5-mcp-plugin\mcp_server\scripts\run_mcp_server.cmd
codex mcp get ue-mcp --json
```

## 4-2) WSL/Linux Codex
```bash
codex mcp remove ue-mcp
codex mcp add ue-mcp -- /path/to/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.sh
codex mcp get ue-mcp --json
```

---

## 5) startup timeout 권장 설정
`~/.codex/config.toml`에 아래 설정 추가/확인:

```toml
[mcp_servers.ue-mcp]
startup_timeout_sec = 60
```

## 5-1) (선택) 다중 UE 인스턴스 selector
동시에 UE를 2개 이상 띄우는 경우, 등록 시 selector를 같이 넘기면 오연결을 줄일 수 있습니다.

먼저 후보 목록/추천 selector를 확인합니다.
- `python -m mcp_server.mcp_stdio --config configs/config.yaml --once-endpoints`
- 출력 JSON의 `candidates[].selector_hint.env` 값을 그대로 사용하면 됩니다.

### Windows
```powershell
$env:UE_MCP_INSTANCE_ID = "<instance_id>"
codex mcp add ue-mcp -- D:\path\to\ue5-mcp-plugin\mcp_server\scripts\run_mcp_server.cmd
```

### WSL/Linux
```bash
UE_MCP_INSTANCE_ID="<instance_id>" codex mcp add ue-mcp -- /path/to/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.sh
```

## 5-2) VSCode Copilot MCP 등록 예시
`settings.json`의 `mcp.servers`에 stdio 명령을 등록합니다.

### Windows
```json
{
  "mcp.servers": {
    "ue-mcp": {
      "type": "stdio",
      "command": "D:/path/to/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.cmd",
      "env": {
        "UE_MCP_INSTANCE_ID": "<instance_id>"
      }
    }
  }
}
```

## 5-3) (선택) 카탈로그 가드 설정
`configs/config.yaml`의 `catalog`에 아래 항목을 설정하면 잘못된 UE 인스턴스 연결을 조기에 차단할 수 있습니다.

```yaml
catalog:
  required_tools: ["system.health", "asset.create"]
  pin_schema_hash: ""
  fail_on_schema_change: false
```

### WSL/Linux
```json
{
  "mcp.servers": {
    "ue-mcp": {
      "type": "stdio",
      "command": "/path/to/ue5-mcp-plugin/mcp_server/scripts/run_mcp_server.sh",
      "env": {
        "UE_MCP_INSTANCE_ID": "<instance_id>"
      }
    }
  }
}
```

---

## 6) 최종 동작 확인
```bash
codex mcp list
```
- `ue-mcp`가 `enabled`면 등록 완료.
- 이후 Codex 에이전트에서 `system.health`, `tools.list` 호출 가능.

---

## 7) 자주 쓰는 명령
- 서버 등록 갱신:
  - `codex mcp remove ue-mcp` 후 `codex mcp add ...` 재실행
- 연결 문제:
  - Unreal 실행 여부
  - `<Project>/Saved/UnrealMCP/connection.json` 갱신 여부
  - `docs/TROUBLESHOOTING.md` 확인
