# UE MCP Server Release Checklist

## 1. 사전 점검
- `configs/config.example.yaml`이 최신 설정 키(`server`, `ue`, `request`, `catalog`, `retry`, `metrics`)를 모두 포함
- `README.md`, `docs/RUNBOOK.md`, `docs/TROUBLESHOOTING.md`가 현재 동작과 일치
- 샘플 클라이언트 `examples/agent_tool_client.py` 실행 가능 확인

## 2. 품질 게이트
- 단위 테스트:
  - `pytest -q`
- 컴파일 체크:
  - `python -m compileall src`
- 기본 스모크:
  - `python -m mcp_server --once-health`
  - `python -m mcp_server --once-tools`
  - `python examples/agent_tool_client.py --list-tools`
  - `python examples/e2e_smoke_runner.py --stream-events`

## 3. UE 연동 스모크(수동)
- UE Editor 5.7 + 플러그인 활성화
- `system.health` 호출 성공
- `mat.instance.params.get` 호출 성공
- `niagara.stack.list` 호출 성공
- `umg.tree.get` 호출 성공
- `--stream-events` 호출 시 `event.*` 출력 확인

## 4. 릴리스 산출물
- 소스 코드(`src/mcp_server`)
- 설정 예시(`configs/config.example.yaml`)
- 문서(`README.md`, `docs/*.md`)
- 샘플 클라이언트(`examples/agent_tool_client.py`)

## 5. 릴리스 후 검증
- 신규 머신에서 문서만으로 30분 내 재현 가능한지 확인
- 연결 실패/타임아웃/툴 미존재 케이스를 트러블슈팅 문서로 복구 가능한지 확인
