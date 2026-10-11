# Magpie Slicer 기능별 변경점

## 1. 문서 목적과 기준

이 문서는 OrcaSlicer 2.5.0 기반 Magpie Slicer `2.5.0.0.8` 후보에 포함할 기능과 현재 작업 트리의 안정화 변경을 기능별로 정리한 기준 문서다.

- 현재 브랜치: `release/2.5.0.0.7`
- 현재 HEAD: `968fca1591e5389fdd186c787554699abb3bc5e4`
- 문서·산출물 재점검 기준일: 2026-08-31 (기존 빌드·실행 기록은 2026-08-28)
- 현재 상태: 추적 파일에 미커밋 변경이 있으며 로컬 설치 후보는 만들었지만 커밋, 푸시, GitHub 릴리스는 수행하지 않았다.
- 공개 릴리스 `v2.5.0.0.7`과 현재 작업 트리는 동일하지 않다. 공개 0.7 설치파일은 당시 Tsunami UI 시작 충돌 수정본이고, 현재 작업 트리에서는 Tsunami 기능을 완전히 제거했다.

최신 상태, 시험 범위의 정정과 빌드 백업은 [2026-08-31 통합 상태·백업 기록](MAGPIE_STATE_AND_BACKUP_2026-08-31.md)을 먼저 읽는다.

상태 표기는 다음 의미로 사용한다.

- **공개 릴리스:** GitHub 릴리스와 설치파일에 포함된 동작.
- **현재 작업 트리:** 현재 소스에 존재하지만 새 설치파일로 릴리스되지 않은 변경.
- **현재 검증:** 현재 작업 트리에서 실제 빌드 또는 슬라이싱 결과가 남아 있는 항목.
- **역사 기록:** 과거 릴리스나 폐기된 설계의 설명이며 현재 동작으로 사용하지 않는 항목.

문서별 역할은 다음과 같다.

| 문서 | 역할 |
| --- | --- |
| `README.md`, `README_EN.md` | 공개 릴리스와 사용자 기능 요약 |
| `release-notes-2.5.0.0.N.md` | 각 공개 설치파일의 변경·해시 역사 |
| 이 문서 | 현재 통합된 기능과 미출시 안정화 상태의 기능별 기준 |
| `MIXED_AUTO_SUPPORT_PLAN.md` | Mixed 판정·병합·페인팅 설계와 상세 검증 |
| `magpie-vulkan-slicer.md` | Vulkan 실행 계약과 CPU fallback 범위 |
| `CODEX_HANDOFF.md` | 세션별 조사·실패·검증의 시간순 기록 |
| `PROJECT_CONTRACTS.md` | 변경 시 지켜야 하는 불변조건과 작업 계약 |

## 2. 제품 버전과 애플리케이션 식별

### 2.1 버전 규칙

Magpie 공개 버전은 Orca의 세 필드 버전 뒤에 Magpie line과 revision을 붙인다.

```text
<Orca major>.<Orca minor>.<Orca patch>.<Magpie line>.<Magpie revision>
```

현재 값은 다음과 같다.

- Orca 기준 버전: `2.5.0`
- Magpie line/revision: `.0.8`
- 현재 설치 후보와 예정 태그: `2.5.0.0.8`, `v2.5.0.0.8`
- SemVer 호환 내부 버전: `2.5.0-modified.0.8`
- Windows 숫자 파일 버전: `2.5.0.8`
- 설치파일 이름: `MagpieSlicer_Windows_Installer_V2.5.0.0.8_x64.exe`

버전의 단일 소유자는 `version.inc`의 `MAGPIE_ORCA_BASE_VERSION`, `MAGPIE_RELEASE_LINE`, `MAGPIE_RELEASE_REVISION`이다. README, 태그, 설치파일 이름에서 별도 기능 카운터를 만들지 않는다.

2026-08-28 로컬 설치 후보는 `build-vulkan/installer/20260828-174933`에 생성했다. 설치파일 SHA-256은 `26DC29254C43ACBC0BD59067BD140A449D3FC68F03B9E447E68BBDD7D0F083AF`이며, NSIS 무결성 검사와 내부 `magpie-slicer.exe`/`MagpieSlicer.dll`의 빌드 산출물 해시 일치 검사를 통과했다. 이 후보는 아직 공개 릴리스가 아니다.

### 2.2 Magpie 애플리케이션 식별

- 표시 이름, 실행 명령, 패키지 이름과 설정 키를 OrcaSlicer와 분리했다.
- 시작 시 upstream Orca 릴리스 확인을 자동 실행하지 않는다. 프로파일·플러그인 동기화와 도움말의 수동 업데이트 확인은 유지한다.
- Vulkan 시험판과 slicing profiler는 별도 앱 이름·키·실행 파일을 사용해 일반판 설정과 충돌하지 않게 했다.
- 시작 스플래시 로고는 지원되지 않는 SVG 임베딩 경로 대신 패키지의 투명 PNG를 직접 읽는다.
- G-code producer 이름은 펌웨어 호환을 위해 `OrcaSlicer`를 유지한다.

## 3. Snapmaker U1 시작 G-code 안전 수정

**공개 릴리스:** 2.5.0.0.1

- 구형 U1 시작 G-code의 안전 보정을 노즐 직경과 무관하게 적용한다.
- 첫 축 이동 전에 현재 네이티브 homing 순서를 사용한다.
- 복사된 구형 시작 코드가 0.4 mm 노즐에서만 별도 경로로 남지 않게 했다.
- 노즐·프로파일 조합 30개 시작 코드 매트릭스가 릴리스 기록에 남아 있다.

관련 소유 영역은 프린터 프로파일, preset 로딩, G-code 시작 순서와 `tools/verification/verify_u1_nozzle_start_matrix.ps1`이다.

## 4. 멀티 노즐 출력

### 4.1 핫엔드별 설정

- 노즐 직경과 선폭을 물리 핫엔드별 벡터로 관리한다.
- 첫 레이어, 외벽, 내벽, 상단, 인필, 서포트 선폭을 핫엔드에 맞게 해석한다.
- 레거시 프로파일의 벡터가 실제 노즐 수보다 짧을 때 안전하게 정규화하는 경로가 있다.
- 재질 ID와 물리 extruder ID를 같은 인덱스로 가정하면 안 된다. 재질 온도는 filament 범위, 노즐 형상은 physical tool 범위가 소유한다.

### 4.2 소구경 노즐 자동 선택과 인터락킹

- 작은 글자, 좁은 외벽과 날카로운 코너를 소구경 노즐로 라우팅한다.
- Classic과 Arachne 벽 생성기의 경계에서 소구경/대구경 벽 수와 간격을 다룬다.
- 인터락킹은 두 노즐 벽의 경계를 레이어별로 이동해 접합 면적을 늘린다.
- 생성된 벽 경로를 G-code 단계에서 사후 삭제·재배정해 형상 오류를 숨기지 않는다.
- 하나의 연속 외벽 중간에서 노즐을 바꾸지 않는다.

### 4.3 대구경 오버라이드와 미리보기

- 지정한 레이어 범위를 대구경 핫엔드로 강제하는 라우팅을 제공한다.
- `Nozzle used` 미리보기는 filament 색과 별도로 실제 사용 노즐을 표시한다.
- 미리보기와 G-code가 서로 다른 도구 결정을 재계산하지 않고 같은 해석 결과를 사용해야 한다.

## 5. 저온 서포트 인터페이스와 온도 드롭 타워

### 5.1 저온 인터페이스 출력 순서

싱글 노즐 출력에서 모델 재질 온도와 서포트 인터페이스 온도를 분리한다.

```text
모델 → 서포트 본체 → 냉각/온도 전환 → 인터페이스 → 재가열 → 다음 모델 경로
```

- 인터페이스 전용 온도와 가열 복귀 시간을 설정한다.
- 지원 하드웨어에서는 AUX fan 냉각과 노즐 wiping을 독립 토글로 제어한다.
- 인터페이스 재질·필라멘트와 물리 노즐 매핑을 구분한다.
- 기능이 꺼졌을 때 기존 Orca G-code 순서와 상태를 바꾸지 않는 것이 핵심 회귀 경계다.

### 5.2 온도 드롭 타워

**공개 릴리스:** 2.5.0.0.2에서 plate 이동 수정

- 플레이트마다 독립적인 X/Y 위치 벡터를 저장한다.
- 출력물 인스턴스를 다른 플레이트로 옮기면 수동 배치 타워도 출력물을 따라간다.
- 목적 플레이트에 기존 수동 위치가 있으면 그 위치를 보존한다.
- 원본 플레이트의 마지막 printable instance가 빠져나간 경우 원본 위치를 정리한다.
- 빈 플레이트, 출력 불가능한 플레이트, 서포트 OFF, 실제 서포트 레이어 없음 조건에서는 preview와 G-code를 만들지 않는다.
- 물리 extruder의 노즐 직경과 extruder offset을 사용하며 printable bed 경계로 위치를 제한한다.
- 사용자가 모델과 겹치게 둔 수동 위치를 자동 회피 명목으로 임의 이동하지 않는다.

릴리스 기록에는 활성화 조건, 노즐·extruder 매핑, 위치, offset과 36설정 매트릭스 검증이 있다.

## 6. 서포트 인터페이스 확장

- 서포트 본체와 인터페이스에 서로 다른 filament를 지정할 수 있다.
- top/bottom interface 레이어와 spacing을 독립 설정한다.
- 삼각형을 포함한 인터페이스 패턴과 선 방향을 지원한다.
- 접촉면부터 계산하는 interface sublayer 범위를 제공하며 범위를 생성된 레이어 수로 제한한다.
- interface smoothing은 실제 하부 support body가 존재해야 하며 공중 interface를 만들면 안 된다.
- 일반 서포트의 `support_wall_count`와 트리의 `tree_support_wall_count`를 별도 설정으로 유지한다.

## 7. Cura 스타일 일반 서포트

### 7.1 사용자 선택지

- `Normal (Cura style, auto)`: 임계각 기반 자동 수요 검출
- `Normal (Cura style, manual)`: enforcer와 painted 영역 기반 생성
- `Cura solid support raft`: 첫 Cura support 레이어를 완전 충전
- `cura_support_join_distance`: 가까운 일반 서포트 영역을 삭제하지 않고 연결하는 거리

### 7.2 생성 계약

- Orca의 overhang 검출과 blocker/enforcer 의미를 유지한다.
- 필요한 support 영역을 하부 레이어로 전달하고 연속 ZigZag 경로를 만든다.
- 파편을 제거해 문제를 숨기지 않고 설정 거리 안의 영역을 연결한다.
- 연결 결과는 원래 영역을 포함해야 하며 모델 충돌 영역을 새로 만들면 안 된다.
- `support_angle`에 따른 선 방향 회전과 `support_wall_count`를 실제 toolpath에 반영한다.
- interface는 support body에 의해 지지되어야 한다.
- 서포트 OFF에서 Cura 생성기를 호출하지 않으며 Cura raft 설정만으로 자동 서포트를 켜지 않는다.

### 7.3 현재 안정화

**현재 작업 트리:**

- Cura를 Mixed normal 채널로 선택했을 때도 `cura_support_join_distance` 행과 값이 활성화된다.
- Mixed에서 사용되는 `support_style` 행을 다시 표시한다.
- normal support wall과 tree wall 설정을 각각 올바른 모드에 표시한다.
- 가까운 영역 연결, 직선·곡선·좁은 형상, bottom interface와 smoothing 검증기가 유지된다.
- Orca native overhang 검출의 automatic polygon과 painted enforcer polygon을 별도 채널로 유지한다.
- CuraEngine 원본처럼 각 하향 전파 레이어를 process resolution으로 단순화해 vertex 누적을 차단한다.
- 정확히 같은 Stanford Bunny 설정에서 현재 Cura CLI는 수정 전 약 142.82초에서 약 37.12초로 단축됐다. 그중 propagation은 약 100.63초에서 약 3.75초로 감소했다.
- 수요 계산, 전파, contact, layer/toolpath 단계에 cancellation checkpoint를 둔다.

## 8. Mixed (auto) 서포트

### 8.1 목적과 설정

Mixed는 Tree Hybrid의 별칭이 아니다. 한 출력물에서 기존 일반 서포트 생성기와 트리 생성기를 동시에 사용한다.

| 설정 | 값과 의미 |
| --- | --- |
| `mixed_normal_support_generator` | Prusa 또는 Cura |
| `mixed_tree_support_style` | Organic, Tree Slim, Tree Strong, Tree Hybrid |
| `mixed_normal_coverage_threshold` | 0~100%, 기본 100% |
| `mixed_selective_merge` | 역치 미만 섬의 도달 가능 부분만 일반으로 분할, 기본 OFF |

`support_type`의 직렬화 키는 `mixed(auto)`이고 stable enum 값은 7이다. GUI combo index와 stable enum 값을 동일시하지 않고 `enum_keys_map`으로 변환한다.

### 8.2 판정 알고리즘

1. 선택한 Prusa 또는 Cura 검출기가 레이어별 vanilla support island를 만든다.
2. 각 island에서 베드로부터 수직 투영으로 도달 가능한 면적을 계산한다.
3. 커버율은 `도달 가능 면적 / 전체 island 면적 × 100`이다.
4. 커버율이 설정 역치 이상이면 island 전체를 일반 서포트에 배정한다.
5. 커버율이 역치 미만이고 Selective Merge가 OFF면 island 전체를 트리에 배정한다.
6. 커버율이 역치 미만이고 Selective Merge가 ON이면 도달 가능한 부분은 일반, 잔여 부분은 트리에 배정한다.

비교는 inclusive다. 정확히 역치와 같은 island는 일반으로 간다. 역치 0%에서는 모든 island가 일반이고 공간 분할을 호출하지 않는다.

인접 레이어의 island를 다시 3차원 union-find로 연결하지 않는다. 상부의 얇은 bridge가 서로 무관한 하부 island를 하나로 합쳐 Stanford Bunny가 전부 tree로 분류되던 구현은 폐기됐다.

### 8.3 두 채널 병합

- 두 엔진의 완성 G-code를 단순 중첩하지 않는다.
- 공통 plan이 normal/tree 입력 mask를 먼저 분리한다.
- normal 생성 결과를 tree obstacle field에 반영한다.
- 같은 Z의 support body와 interface를 명시적인 mixed layer로 병합한다.
- extrusion footprint가 겹치는 구간은 결정적으로 정리한다.
- raft는 한 번만 생성하고 normal 기준 raft와 tree 잔여 경로를 병합한다.
- support body와 interface의 전용 filament 설정을 보존한다.

### 8.4 Mixed 서포트 페인팅

Mixed 전용 생성기 배정은 기존 support enforcer/blocker와 별도 annotation으로 저장한다.

- 미도색: 자동 판정
- 초록: 일반 서포트 강제
- 파랑: 트리 서포트 강제
- 빨강: 서포트 차단
- 노랑: 기존 generic enforcer, 생성기 선택은 자동

우선순위는 `blocker > tree > normal > automatic`이다. 일반으로 칠했더라도 베드에서 도달할 수 없는 부분은 누락하지 않고 tree로 fallback한다. standard 3MF와 Bambu/Orca 3MF round-trip, undo/redo와 mesh remap 경로가 별도 annotation을 보존한다.

### 8.5 현재 안정화와 검증

**현재 작업 트리:**

- vanilla per-layer island 판정으로 all-tree 오분류를 수정했다.
- Selective Merge에서 압출 폭보다 작은 Cura normal mask 조각이 모든 하위 레이어에 증식하던 경로를 조기에 제거했다.
- normal/tree wall count를 각각 해당 채널에 전달한다.
- Tree 또는 Mixed가 포함된 slice는 timing-sensitive topology 때문에 전체를 CPU 경로로 유지한다.

**Mixed 안정화 snapshot 검증:**

- `[SupportMaterial]`: 48 cases, 6,303 assertions 통과 기록
- 전체 CTest: 380/380 통과 기록
- Stanford Bunny, 0.4 mm, 임계각 60도, threshold 50%, Cura+Organic에서 normal/tree 두 채널 확인
- Selective Merge OFF 약 7.3초, ON 약 19.7초 기록
- OFF 3/3, ON 3/3 CLI 조합 통과 기록

**후속 Tree 성능 변경이 포함된 최종 소스 검증:**

- 0.20/0.08 mm, threshold 0/50/95%, Merge OFF/ON 실제 slicing matrix 12/12 통과
- `[Mixed]~[Integration]~[Performance]`: 5 cases, 67 assertions 통과
- `[Tree]~[Integration]`: 1 case, 4 assertions 통과
- 당시에는 이 최종 Tree 변경 뒤 전체 CTest와 전체 `[SupportMaterial]`을 다시 실행하지 않았다.

전체 `[Mixed]` 장시간 suite는 30분 상한에서 CPU-active 상태로 중단됐다. assertion 실패는 관측되지 않았지만 이 실행을 통과로 기록하지 않는다.

**2026-08-28 현재 감사 재검증:**

- 수정 후 전체 Release CTest 393개를 실행해 실패 0건을 확인했다. 환경 필터형 Resin driver 1개는 CTest에서 의도적으로 skip되며 별도 격리 실행 34/34로 보완했다.
- 전체 크기 Stanford Bunny, 0.4 mm 노즐, 임계각 60도 Mixed 최악조건은 11 assertions를 통과했다. Selective Merge ON/OFF 독립 시험도 통과했다.
- support body/interface 실제 폭과 역할 전환을 추적하는 최종 실슬라이싱 행렬은 28/28 통과했다. 단, 이 실행은 기본 `tests/data/overhang.obj`이며 임계각은 60도 19건, 70도 3건, 90도 6건이다. 수정 후 전체 Stanford Bunny 60도 검증으로 대체할 수 없다.
- 최종 Core release-readiness는 한국어 runtime catalog, preset round-trip, multi-nozzle, support unit, Bunny Mixed, Selective Merge, small nozzle, Cura, Tree wall을 포함해 12/12 통과했다.
- Mixed Hybrid Bunny는 같은 15,495개 collision job을 유지하면서 약 151.75초에서 17.22초로 단축됐다. Cura polygon 복잡도 누적을 원 소유 지점에서 줄인 결과이며 Tree topology를 바꾸는 cache는 추가하지 않았다.
- 전체 근거와 수정 전 발견 기록은 `artifacts/code-audit-20260828/findings.md`에 있다. 2026-08-31 문서 감사에서 수정 전 Bunny 28종과 수정 후 overhang 28종을 구분했다. CTest/Resin/Core 결과도 버전 변경·시작 알림 제거 전 실행이며, 최종 설치 후보에는 별도의 패키지 무결성·바이너리 일치 검사를 적용했다.

## 9. Resin style (auto) 서포트

### 9.1 통합 범위

**공개 릴리스:** 2.5.0.0.4

- 최신 PrusaSlicer SLA support point와 tree 전략을 FFF support 경로에 포팅했다.
- Default와 Branching experimental 전략을 선택할 수 있다.
- 생성된 resin tree를 FFF support body, interface, filament, preview와 G-code로 변환한다.
- 모델 침투 깊이는 의도적으로 노출하지 않는다.
- object elevation은 support 토글이 꺼져 있어도 독립 적용할 수 있다.

### 9.2 노출 설정

공통 선택 설정은 다음 세 가지다.

- tree type
- automatic support point density
- enforced region only

Default와 Branching 각각에 다음 16개 parameter 집합을 둔다.

1. pinhead front diameter
2. pinhead width
3. pillar diameter
4. small pillar diameter percent
5. max bridges on pillar
6. max weight on model
7. pillar connection mode
8. support on build plate only
9. pillar widening factor
10. support base diameter
11. support base height
12. support base safety distance
13. critical angle
14. max bridge length
15. max pillar link distance
16. object elevation

따라서 총 35개 Resin editor option을 제공한다. 값이 실제 FFF line width보다 작으면 생성 단계에서 printable 범위로 clamp한다. 얇은 값을 허용하되 pillar 설정 변경 때문에 크래시하면 안 된다.

### 9.3 호환성과 안정화

- 레거시 process preset에 새 Resin key가 없으면 schema default를 materialize한다.
- 명시적으로 저장된 Branching 선택은 보존한다.
- FFF `support_threshold_angle`을 자동 support point 검출에 적용한다.
- threshold 0은 Prusa SLA native detector 의미를 유지한다.
- painted enforcer는 임계각 필터보다 우선한다.
- 모든 설정과 enum label에 한국어 번역을 제공한다.
- object elevation 0은 bed-face point를 제거하지만 실제 overhang support는 유지한다.
- support body와 interface의 서로 다른 filament를 유지한다.
- Default 전략의 max bridge length 0은 무한 bridge 탐색이나 widening을 만들지 않는다.
- Default/Branching 16개 설정과 공통 2개 설정은 설정마다 별도 프로세스로 실행한 34/34 격리 스윕을 통과했다.

릴리스 기록에는 Default/Branching, 0.2/0.4/0.6/0.8 mm 노즐, pillar 0~15 mm, legacy preset 35개 option materialization과 임계각 차등 검증이 있다.

## 10. 트리와 Organic 서포트

### 10.1 임계각과 벽 수

- automatic Tree는 공통 `support_threshold_angle`을 사용한다.
- `tree_support_wall_count`는 0~10 범위다.
- 트렁크와 각 branch는 실제 단면이 수용할 수 있는 벽 수만 사용한다.
- 요청 벽 수를 맞추기 위해 얇은 branch의 최소 직경을 강제로 키우지 않는다.
- Mixed에서는 normal `support_wall_count`와 tree `tree_support_wall_count`를 독립 전달한다.

### 10.2 Tree Slim 성능과 결정성

**현재 작업 트리:** legacy Tree Slim/Strong/Hybrid 경로를 최적화했다. Organic은 `TreeSupport3D` 경로를 유지한다.

- profiler를 전역 객체가 아니라 `TreeSupport` instance별로 소유한다.
- collision/avoidance counter와 timing을 atomic으로 기록한다.
- `MAGPIE_TREE_PROFILE=1`에서 `MAGPIE_TREE_PRECOMPUTE`, `MAGPIE_TREE_PROFILE`을 stderr로 출력한다.
- quantized branch radius별 필요한 최고 object layer를 모은다.
- 독립 collision `(radius, layer)` job을 먼저 병렬 계산한다.
- collision 계산 완료 후 radius별 avoidance를 bottom-up으로 만든다.
- radius 0 collision은 실제 support node가 있는 active layer만 prewarm한다.

Stanford Bunny 기준 Tree 단계는 `50.018초 → 39.646초`, 약 20.7% 감소했고 전체 CLI는 `154.753초 → 142.197초`, 약 8.1% 감소했다. G-code 차이는 timestamp와 object-id comment뿐이며 extrusion/motion 차이는 0으로 기록됐다.

후속 원인분석에서 Mixed가 Tree obstacle로 넘기던 Cura normal polygon이 레이어마다 단순화되지 않아 collision offset 입력 복잡도를 폭증시키는 것을 확인했다. CuraEngine과 동일하게 각 전파 레이어를 process resolution으로 단순화한 뒤 동일 Mixed Hybrid Bunny는 약 17.22초, Tree collision은 약 1.51초가 됐다. collision job 수는 15,495개로 같고 반복 실행의 canonical executable-command stream hash도 동일하다.

Classic Tree의 취소는 collision/avoidance job과 node/toolpath 단계에서 같은 cancellation exception으로 전파한다. 설정 변경이나 취소가 dominant precompute가 끝날 때까지 대기하지 않도록 `[Cancellation]` 회귀시험을 유지한다.

다음 최적화는 결정성을 깨뜨려 금지한다.

- 두 `drop_nodes()` mutation pass의 순서 없는 병렬화
- 제거된 Vulkan AABB shortcut 복원
- worker timing에 따라 node topology가 달라지는 commit
- 측정 10% 기준을 넘지 못한 obstacle union cache 복원

## 11. Bambu Lab A2L 프로파일

**공개 릴리스:** 2.5.0.0.4

- 공식 Bambu Lab A2L machine profile을 통합했다.
- 0.2, 0.4, 0.6, 0.8 mm nozzle variant를 제공한다.
- 17개 process preset과 124개 filament preset을 포함한다.
- Orca 원본과 Bambu Studio template을 비교해 machine G-code, inheritance와 compatible-printer 관계를 검증했다.
- native Orca profile validator로 dependency closure를 로드한다.

프로파일을 업데이트할 때는 한쪽 소스만 복사하지 않고 Orca와 Bambu 양쪽을 다시 비교해야 한다.

## 12. Vulkan 보조 슬라이싱

- 런타임 모드는 Auto, On, Max GPU, Off다.
- CPU geometry가 authoritative fallback이다.
- unsupported device, overflow, unsafe coordinate와 timeout은 partial GPU 결과를 섞지 않고 전체 CPU 결과로 fallback한다.
- exact vertical scanline과 비위상 AABB 후보 질의를 제공한다.
- Tree 또는 Mixed support가 포함된 slice는 branch merge 결정성을 위해 전체 CPU 경로를 사용한다.
- Cura의 same-layer model/support AABB preflight는 CPU에서 수행한다.
- polygon boolean, offset, wall topology, support topology, path ordering, seam, tool assignment와 G-code는 CPU authoritative다.

## 13. LESIC와 Snapmaker 장치 화면

### 13.1 LESIC

- 베드 크기에서 20 mm를 뺀 원통형 calibration model을 중앙 생성한다.
- 바닥 숫자, 외곽 눈금과 내부 brim을 포함한다.
- 작은 베드에서는 글자 크기와 굵기를 줄인다.
- 한 모델에서 온도와 최대 체적 속도를 함께 확인한다.

### 13.2 Snapmaker 장치 화면

- U1용 네이티브 장치 panel을 확장했다.
- camera, layer, temperature, fan과 motion state를 한 화면에서 표시한다.
- PA calibration, bed leveling과 timelapse 선택창은 기본 OFF 상태다.
- 비동기 callback은 panel lifetime과 분리해 stale callback 또는 use-after-free가 발생하지 않게 해야 한다.

## 14. Tsunami 지원 제거

**공개 0.7 릴리스:** Tsunami 기능이 아직 존재했다.

**현재 작업 트리:** 실험적 Tsunami 지원을 제품에서 제거했다.

- `TsunamiSupport.cpp/.hpp`와 CMake source 등록 제거
- `stTsunamiAuto`와 support type 선택지 제거
- `tsunami_*` schema, UI, localization과 개발 probe 제거
- 전용 테스트와 실제 slice script 제거
- 이전 프로젝트의 `support_type = tsunami(auto)`는 `normal(auto)`로 migration
- 이전 `tsunami_*` key는 안전하게 무시
- Mixed와 Resin의 stable enum 값 7과 8은 그대로 보존

코드에 남아 있는 `tsunami(auto)`와 `tsunami_*` 문자열은 legacy migration test와 loader compatibility 용도뿐이다. 역사 문서의 Tsunami 설계를 현재 기능으로 복원하면 안 된다.

## 15. GUI와 한국어 번역

- Mixed, Resin, Cura join distance, normal/tree wall count를 support tab에서 조건부 표시한다.
- Resin Default와 Branching parameter는 선택된 전략에 맞춰 표시한다.
- Resin object elevation은 support OFF에서도 설정 가능하다.
- Organic에서 사용할 수 없는 independent support layer height는 행 자체를 삭제하지 않고 표시한 채 비활성화한다.
- support type GUI는 stable enum 값과 combo index를 `enum_keys_map`으로 변환한다.
- Mixed와 Resin 설정, Resin enum과 tooltip에 한국어 번역을 제공한다.

## 16. 검증 인프라

현재 저장소에는 기능별 검증 스크립트가 있다.

- `verify_mixed_support.ps1`
- `verify_resin_legacy_ui.ps1`
- `verify_cura_support_geometry.ps1`
- `verify_cura_port_regressions.ps1`
- `verify_interface_underside_smoothing.ps1`
- `verify_tree_support_wall_counts.ps1`
- `verify_support_features.ps1`
- `verify_hotend_filament_matrix.ps1`
- `verify_multinozzle_orcacube.ps1`
- `verify_vulkan_*`
- `verify_magpie_release_readiness.ps1`

검증기는 설정 문자열 존재만으로 통과시키지 않고 가능한 경우 다음을 함께 확인해야 한다.

- embedded config 값
- G-code support body/interface role
- finite coordinate
- 레이어 수와 Z 순서
- 실제 extrusion length와 footprint
- 일반/tree channel 존재 여부
- GUI process 생존과 responsiveness
- 빌드 EXE/DLL과 package payload hash

2026-08-28 코드리뷰는 검증기의 독립성과 false-positive 가능성부터 감사했다. 그 결과 60도 Bunny 강제, Mixed 채널·Tree 벽 구조 확인, 실제 Support/Interface 폭 확인, 실행별 Resin 격리를 추가하거나 보강했으며, 검증기 자체 결함은 현재 감사 기록에 제품 결함과 분리해 남겼다.

최종 검증기 수정:

- redirected process의 `ExitCode`가 Windows PowerShell 5.1에서 비어 있는 경우 Catch2 성공 footer와 빈 stderr를 함께 요구한다.
- `;WIDTH`가 역할 전환마다 반복되지 않는 정상 G-code를 처리하도록 현재 활성 폭을 extrusion role에 귀속한다.
- UTF-8 BOM이 없는 스크립트에서도 한국어 runtime MO 검증 문자열이 ANSI code page로 깨지지 않도록 code point로 구성한다.
- skip이 포함된 Catch2 compact 출력은 전체 assertion 성공 줄을 기준으로 판정한다.

## 17. 현재 제한과 미완료 상태

- 현재 지원 안정화, 성능 수정, 검증기 수정은 커밋·푸시되지 않았다.
- 현재 작업 트리를 포함한 로컬 2.5.0.0.8 installer는 2026-08-28 생성했고 2026-08-31 별도 백업했다. 이 작업에서 GitHub release나 push는 하지 않았다.
- 수정 후 최종 후보의 전체 Stanford Bunny/60도/강화된 선폭 검사 행렬은 아직 확인되지 않았다. 수정 전 Bunny 행렬과 수정 후 작은 overhang 모델 행렬을 합쳐 최종 통과로 보고하지 않는다.
- 과거 handoff의 `C:\server\MagpieSlicer-2.5.0.0.7-verified` 복사 기록은 현재 감사의 입력이나 배포 근거로 사용하지 않았다.
- 기존 공개 0.7 installer와 현재 작업 트리의 기능 상태를 혼동하면 안 된다.
- 좌표 기반 GUI 자동 조작은 현재 호스트 계약상 실행하지 않았다. 설정 schema/enum/조건부 표시와 runtime localization은 native/static 검증으로 확인했지만 실제 창에서의 수동 클릭·드래그 확인은 별도 사용자 세션 범위다.
- 실제 프린터 안전성은 G-code와 tool/material mapping을 장비별로 확인해야 한다.

## 18. 기능 소유 파일 지도

| 기능 | 주요 소유 파일 |
| --- | --- |
| 버전·제품명 | `version.inc`, `CMakeLists.txt` |
| support schema·migration | `PrintConfig.cpp/.hpp`, `Preset.cpp` |
| support GUI | `ConfigManipulation.cpp`, `Tab.cpp`, `Field.cpp` |
| Mixed 판정 | `Support/MixedSupportPlan.cpp/.hpp`, `PrintObject.cpp` |
| Mixed painting | `GLGizmoFdmSupports.*`, model/3MF serialization 경로 |
| Cura normal | `Support/CuraStyleSupport.cpp/.hpp`, `SupportCommon.*` |
| Resin | `Support/ResinStyleSupport.*`, SLA support source, `SupportMaterial.cpp` |
| Tree | `Support/TreeSupport.*`, `TreeSupport3D.cpp`, `TreeSupportCommon.hpp` |
| interface·raft·layer merge | `SupportMaterial.cpp/.hpp`, `SupportParameters.hpp` |
| slicing dispatch·Vulkan policy | `PrintObject.cpp`, `BackgroundSlicingProcess.cpp` |
| 온도 드롭 타워 | support configuration, project plate state, G-code emission 경로 |
| 테스트 | `tests/fff_print/test_support_material.cpp`, `tests/libslic3r/*` |
| release 검증 | `scripts/verify_*.ps1` |

## 19. 문서 감사 결과

| 문서 | 감사 결과와 조치 |
| --- | --- |
| `MAGPIE_VERSIONING.md` | 버전 예시를 현재 0.8 후보로 갱신 |
| `MIXED_AUTO_SUPPORT_PLAN.md` | 미검증/검증 완료 모순과 대상 브랜치를 현재 상태로 수정 |
| `MAGPIE_CODE_AUDIT.md` | 과거 commit snapshot임을 명시하고 이 문서로 연결 |
| `CODEX_HANDOFF.md` | routing header를 최신 Tree/Mixed 검증과 현재 package 상태로 갱신 |
| `release-notes-2.5.0.0.7.md` | 역사적 공개 릴리스임을 명시하고 현재 Tsunami 제거 상태로 연결 |
| `README.md`, `README_EN.md` | 공개 릴리스와 미출시 작업 트리의 구분 링크 추가 |
| `magpie-vulkan-slicer.md` | Tree/Mixed 전체 CPU 정책이 현재 소스와 일치함을 확인 |
| `PROJECT_CONTRACTS.md` | changelog가 아닌 불변조건 문서이므로 변경하지 않음 |

문서 수정 전 Markdown 백업은 `.codex-backups/md-feature-review-20260828-064433`에 원래 상대경로 구조로 보존했다.

2026-08-31 재검토에서는 handoff의 수정 대기 문구, 설치파일 없음 모순, 이전 0.7 기능 문서 링크와 시험 범위 오인을 교정했다. 수정 전 문서와 빌드는 `C:\Users\svadmin\Documents\Codex\MagpieBackups\2.5.0.0.8-20260831-094611`에 복사하고 파일별 SHA-256 일치를 확인했다.
