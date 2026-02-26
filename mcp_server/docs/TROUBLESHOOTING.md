# UE MCP Server Troubleshooting

## 연결/실행 문제
- 증상: WSL에서 실행 시 `Connect call failed ('127.0.0.1', 19090)` / `CONNECT_TIMEOUT`
  - 원인: WSL의 `127.0.0.1`은 Windows가 아니라 WSL 자신을 가리킴
  - 조치:
    - UE 플러그인 WS 바인드를 `0.0.0.0`으로 설정(`UnrealMCP.WebSocketTransport.BindAddress`)
    - Python 쪽은 최신 코드면 자동으로 Windows-host gateway(예: `172.x.x.1`)로 fallback 시도

- 증상: `Failed to connect UE WS` 또는 연결 타임아웃
  - 원인: UE Editor 미실행, 포트 불일치, 방화벽 차단
  - 조치: UE 실행 상태 확인, `Saved/UnrealMCP/connection.json` 생성 여부 확인, 포트 점유 확인

- 증상: Codex에서 MCP startup timeout 발생
  - 원인: 잘못된 WS endpoint로 연결 시도 또는 stdio 등록 명령이 환경 의존 경로 사용
  - 조치:
    - MCP 등록 명령을 `scripts/run_mcp_server.sh`로 고정
    - `UE_MCP_CONNECTION_FILE` 또는 `UE_MCP_PROJECT_ROOT`를 설정해 endpoint 자동탐지 사용
    - 필요 시 `~/.codex/config.toml`의 `startup_timeout_sec`를 30 이상으로 설정

- 증상: `UE endpoint selection failed` / `Multiple UE endpoints matched` 에러
  - 원인: 동시에 실행 중인 UE 인스턴스가 2개 이상인데 selector 없이 서버를 시작함
  - 조치:
    - `python -m mcp_server.mcp_stdio --config configs/config.yaml --once-endpoints`로 후보와 selector 힌트 확인
    - 환경변수 또는 인자로 대상 인스턴스를 지정
    - 예: `UE_MCP_INSTANCE_ID=<id>` 또는 `--ue-instance-id <id>`
    - 대안: `UE_MCP_PROJECT_DIR`, `UE_MCP_PROCESS_ID`로도 선택 가능

- 증상: `Unknown tool: ...`
  - 원인: 툴 카탈로그 동기화 이전 호출, 플러그인 쪽 미등록 도구
  - 조치: `--once-tools` 또는 `--list-tools` 실행 후 정확한 도구명 사용

- 증상: `MCP.SERVER.CATALOG_GUARD_FAILED` 또는 `Catalog guard failed` 로그
  - 원인: `catalog.required_tools` 누락, `catalog.pin_schema_hash` 불일치, 런타임 schema 변경(`fail_on_schema_change=true`)
  - 조치:
    - `python -m mcp_server --config configs/config.yaml --once-tools`로 실제 `tools[]/schema_hash` 확인
    - 멀티 인스턴스 환경이면 `--once-endpoints`로 endpoint selector 재지정
    - 설정 파일의 `catalog.required_tools`, `catalog.pin_schema_hash` 값을 현재 대상 UE 인스턴스 기준으로 맞춤

- 증상: 요청이 `timeout`으로 실패
  - 원인: UE 작업 지연 또는 너무 짧은 timeout
  - 조치: `--timeout-ms` 증가, `request.default_timeout_ms` 상향, UE 로그 병행 확인

## 데이터/프로토콜 문제
- 증상: `--params-json must be valid JSON object`
  - 원인: JSON 문법 오류 또는 객체가 아닌 값 전달
  - 조치: 작은따옴표로 전체 감싸고 내부는 큰따옴표 사용
  - 예: `'{"object_path":"/Game/Materials/MI_Test.MI_Test"}'`

- 증상: 스트리밍에서 `event`가 오지 않고 `result`만 출력
  - 원인: 해당 툴이 진행 이벤트를 발행하지 않음
  - 조치: `system.health` 대신 긴 작업 툴로 검증하거나 UE 플러그인 이벤트 발행 여부 확인

- 증상: `e2e_smoke_runner`에서 `status=skipped`가 발생
  - 원인: 자동 탐색(`asset.find`)으로 대상 에셋을 찾지 못함
  - 조치: `--material-path`, `--niagara-path`, `--umg-path`를 직접 지정

- 증상: `asset_lifecycle` 시나리오가 `skipped` 또는 `required tools missing`으로 표시됨
  - 원인: UE 플러그인 `tools.list`에 `asset.create/duplicate/rename/delete`가 아직 노출되지 않음
  - 조치: UE 플러그인 최신 빌드 적용 후 재실행, 임시로 `--skip-asset-lifecycle` 사용

- 증상: `asset_lifecycle`의 `delete.apply`가 `MCP.CONFIRM.TOKEN_INVALID`로 실패
  - 원인: preview의 `confirm_token` 누락/만료 또는 요청 대상 변경
  - 조치: 동일 `object_paths`로 preview를 다시 호출해 새 `confirm_token`으로 apply 재시도

- 증상: `asset_lifecycle`가 성공인데 `MCPRuntimeE2E` 폴더가 비어 있음
  - 원인: 기본 시나리오가 검증 후 `delete.apply`로 생성 에셋을 정리(cleanup)
  - 조치: 유지 검증이 필요하면 `--asset-lifecycle-keep-assets` 옵션으로 실행

- 증상: cleanup/keep 모드를 번갈아 검증할 때 결과 해석이 헷갈림
  - 원인: 단일 명령/수동 실행에서 어떤 모드로 돌렸는지 추적 누락
  - 조치: `python scripts/e2e_modes_runner.py --config configs/config.yaml`로 모드 고정 검증 사용

## 운영 지표 문제
- 증상: `failure_rate`가 높은 상태로 유지
  - 원인: 비정상 요청 입력 또는 지속적 연결 불안정
  - 조치: 실패 요청의 `diagnostics.errors[]` 코드 분석, `retry.*`/WS 연결 상태 조정

- 증상: `p95_duration_ms` 급증
  - 원인: UE 작업량 증가, 에디터 스레드 부하
  - 조치: 호출 부하 분산, 불필요한 연속 요청 축소, 타임아웃/재시도 정책 재조정
