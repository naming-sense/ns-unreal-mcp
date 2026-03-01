# 27) ControlRig E2E 실행 체크리스트

## 목적
- `controlrig.*` 도메인(읽기/수정/포즈/시퀀서/검증)과 `mcp_server` 오케스트레이션이 실제 UE 연결 상태에서 end-to-end로 동작하는지 검증합니다.

## 범위
- 기준: UE 플러그인 + `mcp_server/examples/e2e_smoke_runner.py`
- 대상: `controlrig` 기본 시나리오 + `controlrig_extended` 확장 시나리오
- 부수 검증: 임시 에셋 생성/정리(cleanup), 이벤트 스트림, diagnostics

## 사전 조건
- UE 에디터 실행 중 (`LogUnrealMCP: Started MCP WS event transport ...` 확인)
- `mcp_server/configs/config.yaml` 준비
- `mcp_server/.venv` 준비 및 의존성 설치 완료

## E2E 작업 항목 (실행 순서)

### CR-E2E-01 연결/카탈로그
- [ ] WS 연결 성공 (`MCP.SERVER.CONNECT_TIMEOUT` 없음)
- [ ] `tools.list`에서 `controlrig.*` 툴 노출 확인

### CR-E2E-02 기본 시나리오 (smoke)
- [ ] `controlrig.control.list` 호출 성공
- [ ] `control_count` 집계 확인

### CR-E2E-03 확장 시나리오 - 읽기/조회
- [ ] `controlrig.asset.load`(primary) 성공
- [ ] `controlrig.hierarchy.list` 성공
- [ ] `controlrig.control.list/get` 성공
- [ ] `controlrig.graph.summary` 성공
- [ ] `controlrig.variable.get` 성공

### CR-E2E-04 확장 시나리오 - 수정/포즈
- [ ] `controlrig.control.set` 성공 (제어 대상이 있을 때)
- [ ] `controlrig.control.batch_set` 성공 (제어 대상이 있을 때)
- [ ] `controlrig.control.reset` 성공 (제어 대상이 있을 때)
- [ ] `controlrig.pose.capture` 성공
- [ ] `controlrig.pose.apply` 성공
- [ ] `controlrig.pose.mirror` 성공

### CR-E2E-05 확장 시나리오 - 변수/컴파일/검증
- [ ] `controlrig.variable.set` 성공 (변수가 있을 때)
- [ ] `controlrig.compile` 성공
- [ ] `controlrig.validate` 성공

### CR-E2E-06 확장 시나리오 - 임시 ControlRig 에셋 라이프사이클
- [ ] `controlrig.asset.create` 성공
- [ ] `controlrig.asset.load`(temp) 성공
- [ ] `controlrig.asset.reparent` 성공
- [ ] `controlrig.compile/validate`(temp) 성공

### CR-E2E-07 확장 시나리오 - Sequencer 연동
- [ ] `seq.asset.create`(temp) 성공
- [ ] `seq.binding.add` 성공
- [ ] `controlrig.seq.bind` 성공
- [ ] `controlrig.seq.binding.list` 성공
- [ ] `controlrig.seq.control.list` 성공
- [ ] `controlrig.seq.key.set/update/remove` 성공 (제어 대상이 있을 때)

### CR-E2E-08 정리/관측
- [ ] `asset.delete preview/apply`로 임시 에셋 정리 성공 (`keep-assets=false`)
- [ ] 최종 JSON에서 `controlrig_extended.status == ok`
- [ ] `metrics`/`diagnostics`에 치명 오류 없음

## 실행 명령
### 원클릭 스크립트
```bash
cd mcp_server
./scripts/run_controlrig_e2e.sh
```

Windows PowerShell:
```powershell
cd mcp_server
.\scripts\run_controlrig_e2e.ps1
```

### 수동 실행 (동일 옵션)
```bash
cd mcp_server
.venv/bin/python examples/e2e_smoke_runner.py \
  --config configs/config.yaml \
  --no-auto-discover \
  --material-path /Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New \
  --niagara-path /Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad.NS_JumpPad \
  --umg-path /Game/Input/Touch/UI_Thumbstick.UI_Thumbstick \
  --controlrig-path /Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body \
  --skip-asset-lifecycle \
  --timeout-ms 120000
```

## 완료 기준 (DoD)
- 기본 시나리오 + 확장 시나리오가 모두 `ok`
- 조건부 항목(controls/variables 없음)은 `skipped` + 명시적 reason으로 처리
- cleanup 모드에서 임시 에셋 잔존 없음

## 이번 실행 결과 (2026-03-01)
- 실행한 검증:
  - `python3 -m compileall -q mcp_server/examples/e2e_smoke_runner.py mcp_server/src/mcp_server mcp_server/tests/unit` ✅
  - `.venv/bin/python -m pytest -q tests/unit` → `52 passed` ✅
  - (실패 이력) UE 미기동 상태에서 `MCP.SERVER.CONNECT_TIMEOUT` 확인 ❌
  - (성공) UE 기동 후 아래 명령으로 E2E 완료 ✅
    - `.venv/bin/python examples/e2e_smoke_runner.py --config configs/config.yaml --no-auto-discover --material-path /Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New.MI_Manny_01_New --niagara-path /Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad.NS_JumpPad --umg-path /Game/Input/Touch/UI_Thumbstick.UI_Thumbstick --controlrig-path /Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body --skip-asset-lifecycle --timeout-ms 120000`
- 주요 결과:
  - 최종 payload `ok: true`
  - `require_all: true` 실행도 `ok: true` 확인
  - `controlrig_extended.status: ok`
  - `controlrig_extended.step_count: 31`
  - `seq.key.set/update/remove`, `controlrig.control.set/batch_set/reset`, `pose.capture/apply/mirror` 모두 `ok`
  - `controlrig.validate(temp)`에서 `\"Control Rig has no controls.\"` 경고 1건(예상 동작)
- 실행 시간 참고:
  - `require_all: false` 실행: 전체 약 393초(약 6.5분), ControlRig 확장 시나리오 약 232초
  - `require_all: true` 실행(Sequencer path 명시): 전체 약 370초, ControlRig 확장 시나리오 약 207초
