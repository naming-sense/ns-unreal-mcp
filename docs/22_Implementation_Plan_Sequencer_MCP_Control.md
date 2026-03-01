# 22 — Implementation Plan: Sequencer MCP Control

## 목적
- MCP를 통해 Sequencer(레벨 시퀀스) 작업을 **에이전트가 end-to-end로 수행**할 수 있게 한다.
- 핵심 목표:
  - 시퀀서 에셋 생성/조회/로드
  - 오브젝트 바인딩(Spawnable/Possessable) 및 트랙/섹션 배치
  - 타임라인 키프레임/구간 편집
  - 인스펙터(Details) 조회/패치
  - 컴파일/저장/검증까지 자동화

---

## 사용자 시나리오(우선순위)
1. 빈 `LevelSequence`를 생성하고 카메라/액터를 배치한다.
2. 카메라 컷, Transform, Visibility, Audio, Event 트랙을 추가한다.
3. 섹션 길이/시작 프레임/재생 속도 등을 조절한다.
4. 채널 키(위치/회전/FOV/볼륨 등)를 배치/수정/삭제한다.
5. 현재 시퀀스 구조와 Details를 조회하고 조건부 패치한다.
6. 최종 결과를 저장하고 변경 내역(changeset)로 추적한다.

---

## 범위 정의
### In Scope (1차)
- `ULevelSequence`, `UMovieScene` 기반 편집
- 바인딩/트랙/섹션/키 CRUD
- 섹션 및 바인딩 단위 inspect + object.patch.v2 연계
- 프레임/초 단위 입력 지원(정규화 포함)
- 저장/롤백 친화 설계(변경 패키지 추적)

### Out of Scope (초기 제외)
- 고급 시뮬레이션 기반 평가(런타임 물리 결과 bake)
- 복잡한 Control Rig 그래프 생성 자동화(2차로 분리)
- MRQ(Movie Render Queue) 파이프라인 전체 자동 구성

---

## 툴 설계(초안)
## A. Asset/Sequence 수명주기
- `seq.asset.create`
  - `package_path`, `asset_name`, `display_rate`, `tick_resolution`, `overwrite?`
- `seq.asset.load`
  - `object_path`
- `seq.asset.duplicate`, `seq.asset.rename`, `seq.asset.delete` (기존 asset.* 재사용 가능)

## B. 구조 조회(Inspect)
- `seq.inspect`
  - 시퀀스 메타(프레임레이트, 플레이백 범위, 마스터 트랙 수, 바인딩 수)
- `seq.binding.list`
  - binding id/guid, 이름, spawnable/possessable, 대상 오브젝트 경로
- `seq.track.list`
  - binding별/마스터 트랙별 타입, 채널 요약
- `seq.section.list`
  - 구간 범위(start/end), ease, row index, overlap 정보
- `seq.channel.list`
  - 채널 타입(float/bool/enum/transform axis) + 키 개수

## C. 구조 편집(Create/Mutate)
- `seq.binding.add`
  - `mode=spawnable|possessable`, `target_object_path|actor_path`, `display_name`
- `seq.binding.remove`
- `seq.track.add`
  - 예: `CameraCut`, `Transform`, `Bool`, `Float`, `Audio`, `Event`
- `seq.track.remove`
- `seq.section.add`
  - `track_ref`, `start_frame|start_seconds`, `end_frame|end_seconds`
- `seq.section.patch`
  - 범위 이동/트림/row/easing/blend/활성화
- `seq.section.remove`

## D. 키프레임 편집
- `seq.key.set`
  - `channel_ref`, `frame|time_seconds`, `value`, `interp`
- `seq.key.remove`
- `seq.key.bulk_set`
  - 대량 키 삽입/갱신(성능 최적화용)

## E. 인스펙터 연계
- `seq.object.inspect`
  - 트랙/섹션/채널 UObject 직접 조회
- `seq.object.patch.v2`
  - `object.patch.v2` 래핑(경로 안전성/타입 진단 강화)

## F. 실행/저장/검증
- `seq.playback.patch`
  - playback range, view/work range, loop 옵션
- `seq.save`
  - touched package 저장
- `seq.validate`
  - 깨진 바인딩, 빈 트랙, 음수 범위, 키 정렬 상태 진단

---

## 플러그인 구현 구조 (UE)
## 모듈 분리
- `MCPToolsSequencerReadHandler` (inspect/list)
- `MCPToolsSequencerStructureHandler` (binding/track/section CRUD)
- `MCPToolsSequencerKeyHandler` (key set/remove/bulk)
- `MCPToolsSequencerValidationHandler` (진단/검증)

## 핵심 유틸
- `MCPToolSequencerUtils`
  - 프레임<->초 변환, GUID 안정 참조, track/section resolve
  - 엔진 버전 분기(5.3~5.7)

## 레지스트리 등록
- `MCPToolRegistrySubsystem::RegisterBuiltInTools`에 `seq.*` 도메인 추가
- schema bundle(`schemas_XX_tools.json`) 동시 확장

---

## MCP 서버(파이썬) 확장 계획
## 1) Capability handshake
- `tools.list.capabilities`에 `sequencer_core_v1`, `sequencer_keys_v1` 추가
- 서버는 capability 기반으로 `seq.key.bulk_set` 사용 가능 여부 자동 판단

## 2) 오케스트레이션 helper
- virtual tool: `seq.workflow.compose`
  - 예: `create sequence -> bind actor -> add track/section -> set keys -> save`
- 오류를 단계별(`step_index`, `track_ref`, `binding_id`)로 정규화

## 3) E2E 시나리오
- `sequencer_extended` 시나리오 추가:
  - 시퀀스 생성
  - 바인딩/트랙/섹션/키 구성
  - inspect 검증
  - cleanup 모드/keep 모드 지원

---

## 스키마/응답 규칙
- 모든 식별자는 2중 참조 허용:
  - 사람이 읽는 경로(`object_path`, `binding_name`)
  - 안정 참조(`binding_id`, `track_id`, `section_id`, `channel_id`)
- 시간 입력은 `frame`/`time_seconds` 둘 다 허용, 내부에서 `frame`으로 정규화
- 결과는 항상 `touched_packages`, `job_id`, `diagnostics` 포함

---

## 안전장치/정합성
- 파괴적 연산은 `preview/apply + confirm_token` 적용
- idempotency key 기반 재시도 안정성 보장
- dry-run에서 실제 편집 없이 영향 범위(바인딩/트랙/섹션/키 개수) 반환

---

## 마일스톤
## M1 (기본 조회/생성)
- `seq.asset.create/load`, `seq.inspect`, `seq.binding.list`, `seq.track.list`

## M2 (구조 편집)
- `seq.binding.add/remove`, `seq.track.add/remove`, `seq.section.add/remove/patch`

## M3 (키프레임)
- `seq.key.set/remove`, 채널별 타입 검증, 보간 옵션 지원

## M4 (인스펙터/패치)
- `seq.object.inspect`, `seq.object.patch.v2`, 상세 진단 강화

## M5 (서버 오케스트레이션)
- `seq.workflow.compose`, capability 기반 fallback, 표준 에러 매핑

## M6 (검증/회귀)
- plugin automation + e2e + 다중 UE 인스턴스 환경 검증

---

## 테스트 전략
- 단위 테스트(C++): GUID resolve, frame/time 변환, 키 삽입/삭제 검증
- 자동화 테스트(UE): 시퀀스 생성 -> 트랙/섹션/키 편집 -> inspect/assert
- 서버 단위 테스트(Python): orchestration/fallback/error mapping
- 실E2E: WSL/Windows 혼합, 단일/다중 UE 인스턴스

---

## 리스크와 대응
- 엔진 버전별 MovieScene API 차이
  - 대응: 버전 분기 + 최소 공통 API 우선
- 키 채널 타입 폭증으로 인한 복잡도
  - 대응: 1차 지원 타입 고정 후 점진 확장
- 대량 키 입력 성능
  - 대응: `seq.key.bulk_set` + batched transaction + 선택적 autosave

---

## 완료 기준(Definition of Done)
- 에이전트가 자연어 요청으로:
  1) 시퀀스 생성
  2) 에셋/액터 바인딩
  3) 트랙/섹션/키 배치
  4) 인스펙터 조회/수정
  5) 저장/검증
  을 **MCP 호출만으로 재현 가능**해야 한다.
