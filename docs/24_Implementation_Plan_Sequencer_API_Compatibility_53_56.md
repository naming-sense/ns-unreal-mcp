# 24 — Implementation Plan: Sequencer API Compatibility (UE 5.3~5.6)

## 목적
- 현재 `seq.*` 툴 구현을 **UE 5.3, 5.4, 5.5, 5.6, 5.7**에서 동일하게 빌드/동작하도록 정리한다.
- 개별 핸들러에서 엔진 버전 분기를 직접 처리하지 않고, **공통 호환 계층(compat layer)** 에서 API 차이를 흡수한다.

---

## 참조 기준 (Epic API 문서)
- `UMovieScene`
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/UMovieScene?application_version=5.3
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/UMovieScene?application_version=5.4
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/UMovieScene?application_version=5.5
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/UMovieScene?application_version=5.6
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/UMovieScene?application_version=5.7
- `FMovieSceneBinding`
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/FMovieSceneBinding?application_version=5.3
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/FMovieSceneBinding?application_version=5.7
- `FMovieSceneBoolChannel`
  - https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/MovieScene/Channels/FMovieSceneBoolChannel?application_version=5.3

> 주의: 실제 구현 판단은 문서 + 로컬 엔진 헤더(`MovieScene.h`, `MovieSceneBoolChannel.h`)를 함께 기준으로 한다.

---

## 현재 확인된 차이(대응 필요 항목)
1. `UMovieScene::GetMasterTracks`/`AddMasterTrack`/`RemoveMasterTrack` 사용 불가 구간 존재
   - 공통 API: `GetTracks`/`AddTrack`/`RemoveTrack`
2. `UMovieScene::RemoveBinding` 접근 제한(보호 멤버) 버전 존재
   - 대체: `RemoveSpawnable`/`RemovePossessable` + 바인딩 트랙 제거
3. `UMovieScene::GetBindings()` non-const 접근 deprecated
4. `FMovieSceneBoolChannel::AddKey` 직접 호출 불가 버전 존재
   - 대체: `GetData().AddKey(...)`
5. `FMovieSceneBinding::GetName` deprecated
   - 대체: spawnable/possessable 이름 기반 해석

---

## 설계 원칙
- **원칙 1: 최소 공통 API 우선**
  - 엔진 버전별 신규 API보다 5.3~5.7 공통으로 존재하는 API를 우선 채택.
- **원칙 2: 단일 Compat 진입점**
  - Sequencer 핸들러는 `Compat` 함수만 호출.
- **원칙 3: 컴파일 타임 분기 + 기능 감지**
  - `ENGINE_MAJOR_VERSION/ENGINE_MINOR_VERSION`, `__has_include`, SFINAE를 조합.
- **원칙 4: 동작 동일성 유지**
  - JSON 응답 스키마(`seq.*`)는 버전과 무관하게 동일 유지.

---

## 구현 작업 패키지

### WP1. Compat 계층 신설
- 신규 파일:
  - `Source/UnrealMCPEditor/Private/Tools/Common/MCPSequencerApiCompat.h`
  - `Source/UnrealMCPEditor/Private/Tools/Common/MCPSequencerApiCompat.cpp`
- 제공 함수(초안):
  - `GetGlobalTracks(const UMovieScene*)`
  - `AddGlobalTrack(UMovieScene*, UClass*)`
  - `RemoveTrackSafe(UMovieScene*, UMovieSceneTrack*)`
  - `RemoveBindingSafe(UMovieScene*, const FGuid&)`
  - `GetBindingsConst(const UMovieScene*)`
  - `AddBoolKeySafe(FMovieSceneBoolChannel*, FFrameNumber, bool)`
  - `ResolveBindingDisplayName(UMovieScene*, const FMovieSceneBinding&)`

### WP2. 핸들러 전면 치환
- 대상:
  - `MCPToolsSequencerReadHandler.cpp`
  - `MCPToolsSequencerStructureHandler.cpp`
  - `MCPToolsSequencerValidationHandler.cpp`
  - `MCPToolsSequencerKeyHandler.cpp`
- 목표:
  - 위 파일들에서 `UMovieScene` 버전 민감 API 직접 호출 제거.
  - 모든 호출을 `MCPSequencerApiCompat`로 통일.

### WP3. 경고/폐기 API 정리
- Sequencer 경로에서 `C4996` 경고를 0으로 줄이는 것을 목표로 정리.
- 최소 포함 규칙 유지(버전별 불필요 include 제거).

### WP4. 빌드 매트릭스 검증
- 내부 검증 축:
  - UE 5.3 기반 엔진
  - UE 5.4
  - UE 5.5
  - UE 5.6
  - UE 5.7
- 검증 항목:
  - `UnrealMCPEditor` 모듈 컴파일
  - 링크 성공
  - `tools.list`에 `seq.*` 정상 노출

### WP5. 런타임 회귀 테스트
- `seq.inspect`, `seq.track.list`, `seq.section.list`, `seq.key.set/remove`, `seq.validate`를 버전별 스모크 테스트.
- Python `e2e_smoke_runner`에 `--scenario sequencer`를 버전 매트릭스에서 반복 실행.

### WP6. 문서/운영 반영
- `README`에 Sequencer 호환 버전 범위 명시.
- `docs/23_Sequencer_M1_M6_Implementation_Report.md` 후속 섹션 추가(호환성 결과표).
- Known Issues 템플릿 추가(엔진 커스텀 브랜치 차이 기록).

---

## 리스크 및 대응
- 리스크: 사내 커스텀 엔진이 표준 5.x API와 부분 불일치
  - 대응: `Compat`에 커스텀 분기 훅(매크로) 제공
- 리스크: 문서상 존재하지만 헤더에서 가시성 제약(protected/private)
  - 대응: 문서 대신 실제 헤더/컴파일 결과를 최종 진실(source of truth)로 사용
- 리스크: 버전별 JSON 출력 미세 차이
  - 대응: 공통 contract 테스트 추가(필수 필드 snapshot)

---

## 완료 기준 (Definition of Done)
- UE 5.3~5.7에서 `UnrealMCPEditor`가 동일 소스 기준으로 빌드 성공.
- `seq.*` 핵심 툴 호출이 버전별로 동일 JSON 스키마를 반환.
- Sequencer 경로에서 버전 민감 API 직접 호출이 제거되고 `MCPSequencerApiCompat` 단일 진입점으로 통합.
- 문서/릴리즈 노트에 호환 범위와 예외 케이스가 반영.

---

## 실행 순서(권장)
1. WP1(Compat 계층) → 2) WP2(핸들러 치환) → 3) WP3(경고 정리)
4. WP4(빌드 매트릭스) → 5) WP5(런타임 회귀) → 6) WP6(문서 반영)

---

## 구현 상태 (2026-03-01)
- WP1: 완료 (`MCPSequencerApiCompat` 추가)
- WP2: 완료 (Sequencer 핸들러 compat 치환)
- WP3: 완료 (버전 민감 직접 호출 제거)
- WP4: 부분 완료 (UE 5.7 빌드 통과 + 멀티버전 매트릭스 스크립트 추가)
- WP5: 부분 완료 (Python 단위테스트 통과, UE WS 미연결로 e2e 실호출은 보류)
- WP6: 완료 (실행 리포트 `docs/25_Sequencer_API_Compatibility_WP1_WP6_Report.md` 추가)
