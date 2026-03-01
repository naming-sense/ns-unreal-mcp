# 23 — Sequencer M1~M6 Implementation Report

## 구현 범위 요약
- `seq.*` 도메인 툴을 Unreal 플러그인에 추가했고, MCP 서버에 `seq.workflow.compose` 오케스트레이션 툴을 추가했다.
- 표준 MCP stdio/CLI 경로 모두에서 시퀀서 워크플로우 호출이 가능하다.

## M1 (기본 조회/생성)
- 구현:
  - `seq.asset.create`
  - `seq.asset.load`
  - `seq.inspect`
  - `seq.binding.list`
  - `seq.track.list`
  - `seq.section.list`
  - `seq.channel.list`

## M2 (구조 편집)
- 구현:
  - `seq.binding.add`, `seq.binding.remove`
  - `seq.track.add`, `seq.track.remove`
  - `seq.section.add`, `seq.section.patch`, `seq.section.remove`

## M3 (키프레임)
- 구현:
  - `seq.key.set`, `seq.key.remove`, `seq.key.bulk_set`
- 현재 1차 지원 채널:
  - `UMovieSceneFloatSection` (float)
  - `UMovieSceneDoubleSection` (double, 헤더 존재 시)
  - `UMovieSceneBoolSection` (bool, 헤더 존재 시)

## M4 (인스펙터/패치)
- 구현:
  - `seq.object.inspect` → `object.inspect` 경로 재사용
  - `seq.object.patch.v2` → `object.patch.v2` 경로 재사용

## M5 (서버 오케스트레이션)
- 구현:
  - virtual tool `seq.workflow.compose`
  - capability-aware 처리:
    - `sequencer_keys_v1` 미지원 시 `key.bulk_set` -> `seq.key.set` fallback
  - stdio(`mcp_stdio.py`) + CLI(`app.py`) 모두 virtual tool 노출/호출 지원

## M6 (검증)
- Python 단위 테스트:
  - 전체 `49 passed`
  - 신규 추가:
    - `tests/unit/test_sequencer_orchestrator.py`
    - `test_mcp_stdio.py`의 sequencer virtual tool 경로 검증
- E2E 스모크:
  - `examples/e2e_smoke_runner.py`에 `sequencer` 기본 시나리오(`seq.inspect`) 추가
  - `sequencer_extended` 시나리오 추가
    - create -> binding/track/section -> key.set -> playback.patch -> save -> validate -> cleanup

## 현재 제약(명시)
- `seq.key.*`는 1차로 단일 section 채널 모델(`channel_index=0`) 중심 구현이다.
- 고급 track/channel 타입(예: 특수 커스텀 채널)의 광범위 지원은 후속 확장 대상으로 남겨둔다.
