# Mixed (Auto) Support Implementation Plan

Status: implementation and targeted runtime verification complete; installer/release work not performed

Target branch: `release/2.5.0.0.3`

## 0. 승인용 요약

- 구현을 시작했으며 설정/UI, 공통 수요 planner, 필터된 생성기, 혼합 레이어 병합 및 focused test 코드까지 반영했다. 빌드·런타임 검증은 아직 실행하지 않았다.
- Mixed raft는 일반 채널의 raft path를 기준으로 삼고, 트리 채널 path에서 기준 path의 실제 압출 폭과 겹치는 구간을 잘라낸 뒤 잔여 path만 합친다. 따라서 부분 겹침에서도 어느 한 footprint를 버리지 않으며 동일 위치 중복 압출을 만들지 않는다.
- 기존 `Tree Hybrid`는 트리 엔진 안에서 큰 평면 아래에 일반형 노드를 섞는 스타일일 뿐, Prusa/Cura 일반 서포트와 트리 서포트를 함께 사용하는 기능이 아니다.
- 새 `Mixed (Auto)`는 서포트 필요 영역을 3차원 연결 덩어리 단위로 나눈 뒤 기본적으로 각 덩어리 전체를 일반 또는 트리 중 하나에만 배정한다.
- `Selective merge`를 켜면 역치 미만인 덩어리 내부를 다시 공간 분할하여, 베드에서 수직 도달 가능한 부분은 일반으로 만들고 막힌 잔여 부분만 트리로 만든다. 기본값은 OFF다.
- 일반 커버율은 `베드에서 수직으로 올라올 때 하부 출력물에 막히지 않는 면적 / 해당 덩어리 전체 면적 * 100`으로 한 번만 계산한다.
- 커버율이 역치와 같거나 크면 일반, 작으면 트리로 배정한다. 따라서 역치 80%에서 정확히 20%가 막힌 덩어리는 일반 서포트가 된다.
- UI에는 `일반 서포트 생성기(Prusa/Cura)`, `트리 스타일(Organic/Slim/Strong/Tree Hybrid)`, `일반 서포트 커버 퍼센테이지 역치(0~100%)`를 추가한다.
- 예전 프로젝트의 `hybrid(auto)`는 이미 `tree(auto) + tree_hybrid` 호환값이므로 새 모드는 내부적으로 `mixed(auto)`를 사용한다.
- 두 엔진의 완성된 G-code를 단순 중첩하지 않는다. 공통 판정 계획으로 입력 영역을 분리하고, 같은 Z의 서포트 레이어를 명시적인 혼합 상태로 안전하게 합친다.
- 1차 레이어, 브림, 리트랙션, 미리보기, 캐시, 이종 서포트/인터페이스 재질까지 혼합 레이어 소비자를 전부 점검한다.
- 구현과 빌드·테스트는 사용자의 완료 지시에 따라 수행했다. 설치파일 생성과 푸시는 이 작업 범위에 포함하지 않았다.

## 1. Goal

Add a new support type, **Mixed (Auto)**, which normally assigns each connected support-demand region to exactly one of the existing support generators, with an optional selective spatial split for below-threshold regions:

- normal support when a sufficient percentage of the region can be reached by vertical support rising from the build plate;
- tree support when that percentage is below the configured threshold.

The feature shall reuse Orca's existing Prusa-style normal, Cura-style normal, and tree support generators. It shall not introduce a fourth geometry engine or reinterpret the existing Tree Hybrid style as this feature.

## 2. User-facing settings

When `Support type = Mixed (Auto)` is selected, show these two additional dropdowns, one percentage setting, and one toggle directly below it:

1. `Normal support generator`
   - Prusa
   - Cura
2. `Tree support style`
   - Organic
   - Tree Slim
   - Tree Strong
   - Tree Hybrid
3. `Normal support coverage threshold`
   - range: 0% through 100%
   - unit: percentage points
   - proposed default: 100%
4. `Selective merge`
   - default: off
   - off: a below-threshold connected component goes entirely to tree support;
   - on: a below-threshold component is split spatially, with its vertically reachable portion assigned to normal support and its remaining portion assigned to tree support.

The normal-generator dropdown selects the actual normal geometry engine, not merely an infill pattern. Existing common support settings such as filament, interface, spacing, and base pattern continue to apply. The generic `support_style` control is hidden while Mixed (Auto) is active because the mixed-mode tree style has its own unambiguous selector and the normal channel uses the selected engine's default style behavior.

Proposed stable serialization keys:

- support type value: `mixed(auto)`
- `mixed_normal_support_generator = prusa | cura`
- `mixed_tree_support_style = organic | tree_slim | tree_strong | tree_hybrid`
- `mixed_normal_coverage_threshold = 0..100`
- `mixed_selective_merge = 0 | 1`

`mixed(auto)` is intentional. The historical value `hybrid(auto)` already means legacy Tree Hybrid and is migrated by Orca to `tree(auto)` plus `tree_hybrid`; reusing that value would silently change old project files.

## 3. Exact classification rule

### 3.1 Canonical demand

Detect automatic support-demand polygons once with the selected normal generator's detector, including support enforcers and blockers. Prusa selection uses the Prusa demand detector; Cura selection uses the Cura full-overhang detector. Both output channels consume masks derived from this same canonical demand. The normal and tree generators must not independently decide which region they own.

All polygon operations remain in Orca's scaled integer coordinate system.

### 3.2 Connected region

A region is a 3D connected component of support-demand polygons:

- polygons on the same layer are connected when their dilated shapes overlap;
- polygons on adjacent layers are connected when their dilated shapes overlap;
- the initial dilation distance is one effective support extrusion width;
- polygons separated by more than one layer are not joined merely because their XY projections overlap.

The existing `OverhangCluster` implementations in `SupportMaterial.cpp` and `TreeSupport.cpp` are reference behavior only. They insert into the first matching cluster and do not merge two existing clusters bridged by a later polygon, so the Mixed planner must use a deterministic union-find implementation instead of copying either implementation.

### 3.3 Build-plate reachability

For demand polygon `D` on object layer `L`:

- `blocked(L)` is the cumulative XY union of all printable object slices below `L`;
- `reachable(D, L) = D - blocked(L)`;
- `reachable_area` is the area of `reachable(D, L)`;
- `demand_area` is the area of `D`.

For a connected component `C`:

```text
coverage(C) = 100 * sum(reachable_area) / sum(demand_area)
```

Classification is inclusive:

```text
coverage(C) >= threshold  -> entire component goes to normal support
coverage(C) <  threshold  -> entire component goes to tree support
```

With `Selective merge = on`, the inclusive threshold decision remains the first gate:

```text
coverage(C) >= threshold -> entire component goes to normal support
coverage(C) <  threshold -> normal = reachable(C), tree = demand(C) - reachable(C)
```

This preserves the established threshold meaning. At threshold 80%, a component with exactly 80% reachable area still goes entirely to normal support. A component with 79% reachable area is split only when Selective merge is enabled; when disabled, it goes entirely to tree support. Threshold 0% therefore always produces normal support and never invokes spatial splitting.

The split masks must be an exact partition of canonical demand after polygon normalization:

```text
union(normal_mask, tree_mask) == canonical_demand
intersection(normal_mask, tree_mask) == empty
```

Any physical transition allowance belongs to downstream support-body/toolpath generation and must not make the canonical ownership masks overlap.

Examples:

- threshold 80%, 20% blocked: coverage is 80%, so the entire component uses normal support;
- threshold 100%: only completely vertically reachable components use normal support;
- threshold 0%: every non-empty component uses normal support.

The cumulative lower-model shadow used here must be the same function used to restrict the normal channel to build-plate-origin support. One geometric quantity must have one owner and one implementation.

### 3.4 Numerical boundary

Keep polygon coordinates scaled and accumulate the existing `double` polygon-area results into `long double`. Compare by cross multiplication instead of dividing: `reachable_area * 100 >= threshold * demand_area`. Add an explicit relative tolerance only at the final comparison. Tests must cover threshold minus epsilon, exact threshold, and threshold plus epsilon.

## 4. Existing code and external research

### 4.1 Reusable Orca code

- `PrintObjectSupportMaterial::buildplate_covered()` already computes the cumulative lower-object shadow used by build-plate-only normal support.
- Cura-style normal support already has `keep_buildplate_connected_support()` for retaining support bodies connected to the plate.
- Tree support already has 3D collision, avoidance, placeable-area, and build-plate routing through `TreeModelVolumes`.
- Both normal and classic tree code contain an `OverhangCluster` implementation that groups neighboring overhangs across layers.
- Orca already has three separate dispatch paths: Prusa normal, Cura normal, and tree.

What Orca does not have is a planner that assigns one connected demand component to one of two generators and safely merges both generators' layer output.

### 4.2 Official upstream findings

- Bambu Studio describes existing Tree Hybrid as tree support with normal nodes under large flat overhangs, not as two selectable generators: https://github.com/bambulab/BambuStudio/issues/2669
- Orca users have separately requested simultaneous tree and normal support; the documented response points to Tree Hybrid but notes that it provides no such control: https://github.com/OrcaSlicer/OrcaSlicer/discussions/8076
- Bambu's source explicitly states that Tree Hybrid contains normal nodes: https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/Support/TreeSupport.cpp
- CuraEngine's normal support pipeline detects overhangs and propagates support areas downward, which is conceptually reusable but does not provide the requested per-component percentage classifier: https://github.com/Ultimaker/CuraEngine/blob/main/src/support.cpp
- Cura's tree implementation provides the collision/influence-area machinery already ported into Orca: https://github.com/Ultimaker/CuraEngine/blob/main/src/TreeSupport.cpp
- A reported accidental simultaneous normal/tree result in Bambu demonstrates why independently generated toolpaths must not simply be overlaid: https://github.com/bambulab/BambuStudio/issues/9265

Conclusion: reuse Orca's engines and geometry primitives, but implement the component planner and mixed-output contract locally. There is no official upstream implementation matching the requested threshold semantics that can be cleanly imported.

## 5. Architecture decision

### Alternative A: extend existing Tree Hybrid

Advantages: smallest code change; already mixes circular and polygonal nodes.

Rejected because it cannot select the real Prusa or Cura normal generator and does not implement percentage-based component ownership.

### Alternative B: run both full generators and concatenate their final toolpaths

Advantages: maximum apparent reuse and quick initial prototype.

Rejected because both engines currently assume exclusive ownership of `PrintObject::support_layers()`. Their propagated bodies may overlap even when contact demand is disjoint, and first-layer islands, brims, retraction, serialization, and layer indexing assume one support type per layer.

### Alternative C: immutable plan plus filtered generators and an explicit mixed-layer merge

Selected approach:

1. Create an immutable `MixedSupportPlan` containing per-layer normal-demand and tree-demand masks plus component classification metadata. Selective merge may give both masks geometry from one below-threshold component, but every point still has exactly one owner.
2. Let the selected normal generator consume only the normal mask.
3. Let the selected tree generator consume only the tree mask.
4. Preserve each generator's geometry and path behavior below its assigned contacts.
5. Merge generated support layers by `print_z` through one ownership-safe function.
6. Represent layers containing both channels explicitly as mixed instead of pretending they are normal or tree.
7. Detect and resolve propagated-body overlap before accepting merged paths.

This is larger than Alternative A, but it is the smallest approach that meets the requested semantics without corrupting old modes.

## 6. Planned implementation stages

### Stage 1: schema and UI, behavior still disabled

Likely files:

- `src/libslic3r/PrintConfig.hpp`
- `src/libslic3r/PrintConfig.cpp`
- `src/libslic3r/PrintObject.cpp` for invalidation
- `src/slic3r/GUI/Tab.cpp`
- `src/slic3r/GUI/ConfigManipulation.cpp`

Work:

- add `stMixedAuto` and `is_mixed()`;
- add typed enums for the two dropdowns;
- add the bounded percentage option;
- add the default-off selective-merge boolean;
- add visibility/enabled-state rules;
- add serialization defaults and invalidation coverage;
- preserve migration of legacy `hybrid(auto)` unchanged.

Gate: old profiles load identically and feature-off behavior is byte-for-byte configuration-compatible.

### Stage 2: shared demand clustering and coverage planner

New focused files are preferred, for example:

- `src/libslic3r/Support/MixedSupportPlan.hpp`
- `src/libslic3r/Support/MixedSupportPlan.cpp`

Work:

- replace the order-dependent existing clustering behavior with a deterministic union-find planner;
- expose one cumulative build-plate-shadow function;
- build normal/tree demand masks from the exact formula in section 3;
- when selective merge is enabled, spatially partition only below-threshold components into reachable normal and unreachable tree masks;
- log component ID, demand area, reachable area, coverage, threshold, and assignment at debug level;
- make the plan immutable after construction.

Gate: pure geometry tests pass for disconnected, adjacent-layer, undercut, 0%, exact-boundary, and 100% cases.

### Stage 3: filtered generator entry points

Likely files:

- `src/libslic3r/Support/SupportMaterial.hpp/.cpp`
- `src/libslic3r/Support/CuraStyleSupport.hpp/.cpp`
- `src/libslic3r/Support/TreeSupport.hpp/.cpp`
- `src/libslic3r/Support/TreeSupport3D.hpp/.cpp`

Work:

- add optional immutable demand masks to generator entry points;
- intersect Prusa normal contacts with the normal mask after annotations but before propagation;
- filter Cura automatic and enforced demand consistently before propagation;
- feed the tree mask to both classic and Organic tree paths;
- allow an explicit tree-style override without mutating the object's saved configuration;
- leave every non-mixed call on the current code path.

Gate: with an all-normal mask, output matches the selected normal engine; with an all-tree mask, output matches the selected tree style, subject only to the new mixed metadata.

### Stage 4: safe mixed-layer assembly

Likely files:

- `src/libslic3r/Print.hpp`
- `src/libslic3r/PrintObject.cpp`
- `src/libslic3r/Layer.hpp`
- support layer generation utilities

Work:

- generate each channel into separately owned layer collections;
- merge by `print_z`, preserving layer height, interface index, islands, slices, base areas, and extrusion entities;
- add an explicit mixed support-layer type or equivalent channel flags;
- prevent duplicate paths where propagated normal and tree bodies meet;
- keep layer IDs sorted and contiguous after the merge;
- avoid sharing or double-deleting raw `SupportLayer*` ownership.

Gate: no same-Z duplicate support layers, no overlapping extrusion paths, and no dangling layer ownership.

### Stage 5: downstream mixed-support consumers

Audit and update:

- first-layer island collection;
- brim generation;
- support retraction decisions;
- preview coloring and support-type serialization;
- support filament and interface filament assignment;
- cache save/restore;
- support-layer simplification and G-code ordering.

Gate: a mixed first layer includes both footprints, and neither channel is lost because a consumer took a two-way normal/tree branch.

### Stage 6: automated and manual verification

Automated matrix:

- normal generator: Prusa, Cura;
- tree style: Organic, Slim, Strong, Hybrid;
- threshold: 0, 80, 100, and boundary epsilon cases;
- region geometry: fully reachable, partially blocked, fully blocked, multiple disconnected components, adjacent-layer component, same-XY non-adjacent components;
- annotations: blocker, enforcer, painted support;
- `On build plate only`: off and on;
- raft: off and on;
- support/interface filament: same and different material;
- nozzle profiles: 0.2, 0.4, 0.6, and 0.8 mm;
- normal styles and base patterns that remain applicable;
- project save/reload and old-profile migration.

G-code checks:

- every extrusion remains within the configured machine boundary;
- no duplicate XY extrusion on the same layer from the two channels;
- support and interface tools match their configured filaments;
- first-layer/brim paths contain both required support footprints;
- changing a setting invalidates and regenerates support;
- feature-off G-code remains unchanged for existing support types.

Build, test execution, installer creation, release commit, and push are outside this planning stage and require a separate explicit instruction under the repository contract.

## 7. Known risks and controls

- **Different overhang detectors:** solve by one canonical demand plan, then filter both generators from it.
- **Lower support bodies converge:** detect per-layer overlap before toolpath merge and establish deterministic ownership; never concatenate blindly. Selective contact ownership does not waive this rule.
- **Organic tree has a separate detector:** add the same mask input to its explicit-threshold and smart-threshold paths.
- **Legacy `hybrid(auto)` collision:** serialize the new mode as `mixed(auto)` and retain the old migration rule.
- **Single support type per layer:** replace binary assumptions with an explicit mixed state and audit every consumer.
- **Raft ownership:** use one shared raft owner; do not let both channels generate duplicate rafts.
- **Cancellation and cache state:** ensure both channel runs use the existing cancellation callback and clear only their own temporary state.
- **Performance:** cache cumulative shadows, cluster bounds, and per-layer masks; do not recompute polygon unions inside each component comparison.

## 8. Approval decisions before coding

The implementation can proceed with the proposed defaults unless changed:

1. default coverage threshold: 100%;
2. normal choices: Prusa and Cura only;
3. tree choices: Organic, Slim, Strong, and Tree Hybrid;
4. comparison at the threshold is inclusive (`>=` means normal);
5. classification unit is one 3D connected demand component;
6. new serialized support type is `mixed(auto)`;
7. in Mixed (Auto), the normal channel is always evaluated and generated as vertical build-plate-origin support; the existing `On build plate only` option additionally controls whether tree branches may terminate on the model.

## 9. 실제 코드 기준 구조 감사 결과

이 절부터는 구현용 상세 명세이며 앞 절의 고수준 설명보다 우선한다.

### 9.1 현재 생성 경로

| 채널 | 진입점 | 영역/경로 생성 | 최종 레이어 기록 |
| --- | --- | --- | --- |
| Prusa 일반 | `PrintObjectSupportMaterial::generate()` | `SupportMaterial.cpp` + `SupportCommon.cpp` | `PrintObject::support_layers()` 직접 사용 |
| Cura 일반 | `CuraStyleSupportGenerator::generate()` | `CuraStyleSupport.cpp` + `SupportCommon.cpp` | `PrintObject::support_layers()` 직접 사용 |
| Organic 트리 | `generate_tree_support_3D()` | `TreeSupport3D.cpp` + `SupportCommon.cpp` | `PrintObject::support_layers()` 직접 사용 |
| Slim/Strong/Tree Hybrid | `TreeSupport::generate()` | `TreeSupport.cpp` 독자 레이어/툴패스 경로 | `PrintObject::support_layers()` 직접 사용 |

따라서 두 생성기를 같은 객체에 연속 호출하는 것만으로는 구현할 수 없다. `generate_support_layers()`는 서포트 레이어가 비어 있다고 단언하고, 네 경로 모두 최종 컨테이너의 독점 소유를 전제로 한다.

### 9.2 숨은 전역 분기

생성기 입구 외에도 다음 코드가 저장된 `support_type`을 다시 읽는다.

- `SupportParameters`: 일반/트리 기본 스타일 결정;
- `SupportCommon`: Cura 첫 레이어 정렬, 밀도, Hollow 처리;
- `Print`: Organic 가변 레이어 제한과 트리 설정 검증;
- `ModelArrange`: 배치 여유 폭과 트리 존재 여부;
- `3DScene` 및 support gizmo: 임계각 표시와 트리 편집 상태;
- `GCode`: 트리 서포트 wipe 및 리트랙션 최적화;
- `Brim`과 `Print::first_layer_islands()`: 첫 레이어 일반/트리 footprint 선택.

`is_tree(stMixedAuto) == true`처럼 기존 헬퍼 하나를 넓혀 버리면 일반 채널까지 트리로 취급된다. Mixed에는 용도별 헬퍼와 실행 문맥이 필요하다.

### 9.3 최종 레이어의 이진 가정

`SupportLayer::support_type`은 현재 `stInnerNormal` 또는 `stInnerTree`뿐이다. Mixed에서는 `stInnerMixed`를 추가하고 다음 질의 함수로 소비처를 바꾼다.

```cpp
bool has_normal_channel(const SupportLayer &);
bool has_tree_channel(const SupportLayer &);
```

기존 직렬화 값 0과 1은 보존하고 Mixed만 새 값 2를 사용한다. 기존 캐시와 프로젝트를 다시 읽을 때 의미가 바뀌면 안 된다.

## 10. 확정 구현 구조

### 10.1 설정 타입

추가 타입과 옵션은 다음처럼 독립적인 typed enum으로 둔다.

```cpp
enum SupportType {
    // existing values unchanged
    stMixedAuto
};

enum MixedNormalGenerator {
    mngPrusa,
    mngCura
};

enum MixedTreeStyle {
    mtsOrganic,
    mtsSlim,
    mtsStrong,
    mtsTreeHybrid
};
```

의미가 다른 기존 `SupportMaterialStyle`을 Mixed 설정 저장 타입으로 재사용하지 않는다. 실행 직전에만 `MixedTreeStyle -> SupportMaterialStyle` 순수 변환 함수를 사용한다.

용도별 헬퍼를 둔다.

```cpp
is_mixed(type)
is_exclusive_tree(type)
uses_tree_channel(type)
uses_normal_channel(type)
is_automatic_support(type)
```

기존 `is_tree()`의 의미는 바꾸지 않거나 `is_exclusive_tree()`로 명시적으로 이름을 정리한다. 전역 일괄 치환은 하지 않고 9.2의 각 호출부 의미를 표로 검토한 뒤 바꾼다.

### 10.2 엔진 실행 문맥

객체 설정을 임시로 바꾸지 않고, 채널별 불변 문맥을 명시적으로 넘긴다.

```cpp
enum class SupportGeometryEngine {
    PrusaNormal,
    CuraNormal,
    OrganicTree,
    ClassicTree
};

struct SupportGenerationContext {
    SupportGeometryEngine engine;
    SupportMaterialStyle effective_style;
    const SupportDemandMask *demand_mask;
    const SupportObstacleField *extra_obstacles;
    bool emit_raft_paths;
};
```

`SupportParameters`와 `generate_support_toolpaths()`도 필요한 엔진 의미를 이 문맥에서 받는다. `config.support_type`을 다시 읽어 Cura 여부를 판정하는 코드는 문맥의 `engine == CuraNormal`로 교체한다. 기존 호출자는 기본 문맥을 생성하는 overload를 사용해 현재 동작을 그대로 유지한다.

### 10.3 공통 서포트 수요 모델

```cpp
struct SupportDemandLayer {
    Polygons automatic;
    Polygons enforced;
};

struct MixedComponentDecision {
    size_t id;
    std::vector<size_t> polygon_ids;
    long double demand_area;
    long double reachable_area;
    double coverage_percent;
    SupportChannel assigned_channel;
};

struct MixedSupportPlan {
    std::vector<SupportDemandLayer> source_demand;
    std::vector<Polygons> normal_mask;
    std::vector<Polygons> tree_mask;
    std::vector<Polygons> buildplate_shadow;
    std::vector<MixedComponentDecision> decisions;
};
```

선택한 일반 생성기가 수요의 기준 소유자다.

- Prusa 선택: `top_contact_layers()` 안의 자동 오버행 검출 단계를 순수 함수로 추출한다.
- Cura 선택: `compute_cura_style_full_overhangs()` 결과를 공개된 내부 함수로 받는다.
- blocker, enforcer, bridge 제외가 끝난 수요를 planner 입력으로 사용한다.
- `support_on_build_plate_only`로 잘라내기 전의 수요를 사용해야 한다. 먼저 잘라내면 커버 불가능한 영역 자체가 사라져 트리로 배정할 수 없다.
- 트리 생성기는 독자 오버행 검출을 다시 하지 않고 `tree_mask`만 소비한다.
- 자동 수요와 강제 수요는 마스크 안에서도 구분해 각 엔진의 enforcer 전파 규칙을 보존한다.

### 10.4 연결요소 알고리즘

기존 첫 일치 클러스터 삽입 대신 다음 결정적 알고리즘을 사용한다.

1. 모든 수요 폴리곤에 안정적인 ID를 `layer, bbox min, bbox max, 원래 순번` 순으로 부여한다.
2. 유효 서포트 extrusion width만큼 한 번 dilate하고 bbox를 저장한다.
3. 레이어별 R-tree를 만들어 같은 레이어와 바로 인접한 레이어의 bbox 후보만 조회한다.
4. bbox 후보끼리 실제 폴리곤 overlap을 검사한다.
5. 겹치면 disjoint-set union으로 묶는다.
6. union 결과 root를 안정 정렬해 component ID를 확정한다.

이 방식은 뒤에 들어온 폴리곤이 두 기존 덩어리를 잇는 경우도 올바르게 합치며, 전수 비교를 피한다.

### 10.5 커버율 계산

누적 하부 출력물 그림자는 별도 순수 함수 하나가 소유한다.

```text
shadow[0] = empty
shadow[L] = union(shadow[L-1], offset(object_slice[L-1], 0.01 mm))
```

각 폴리곤은 bbox가 그림자와 겹치지 않으면 boolean difference 없이 전체 면적을 reachable로 처리한다. 겹칠 때만 `diff(demand, shadow[layer])`를 실행한다.

비교는 나눗셈 없이 수행한다.

```text
reachable_area * 100 >= threshold * demand_area
```

면적은 scaled-coordinate polygon의 기존 `double` 결과를 `long double`에 누적한다. 절대 epsilon이 아니라 component 면적에 비례한 상대 허용오차를 마지막 비교 한 곳에서만 적용한다.

빠른 경로:

- threshold 0: 모든 component를 일반으로 배정하고 shadow difference를 생략;
- 빈 수요: 두 생성기를 모두 생략;
- 한 채널 mask가 비면 해당 생성기 전체를 생략.

### 10.6 채널별 격리 실행과 소유권

새 RAII 컨테이너를 둔다.

```cpp
class OwnedSupportLayers {
public:
    SupportLayerPtrs layers;
    ~OwnedSupportLayers(); // 설치되지 않은 포인터만 삭제
};
```

`PrintObject`에는 다음과 같은 명시적 소유권 이동 API만 추가한다.

```cpp
OwnedSupportLayers take_support_layers();
void install_support_layers(OwnedSupportLayers &&);
void clear_support_detection_state();
```

실행 순서:

1. planner 생성;
2. normal mask가 있으면 일반 채널 단독 실행;
3. 결과를 `take_support_layers()`로 분리;
4. 일반 footprint로 트리 obstacle field 생성;
5. tree mask가 있으면 트리 채널 단독 실행;
6. 결과를 분리;
7. 같은 Z 레이어를 병합하고 최종 컨테이너를 한 번만 설치.

취소나 예외가 발생하면 RAII 컨테이너가 임시 포인터를 정리한다. 기존 `clear_support_layers()`로 임시 결과를 지우지 않는다. 이 함수는 sharp tail/cantilever까지 함께 비워 다른 채널의 입력 상태를 깨뜨릴 수 있다.

### 10.7 두 채널 충돌 방지

접촉 수요가 분리돼도 아래로 전파된 일반 기둥과 트리 가지는 만날 수 있다. Classic 계열은 routing 단계의 장애물 회피를 쓰고, Organic은 같은 Z의 실제 압출 envelope를 기준으로 후단에서 normal 소유 영역을 잘라내 연결한다.

장애물 footprint는 넓은 계획 영역인 `support_islands`가 아니라 `support_fills.polygons_covered_by_width()`로 얻은 실제 압출 폭만 Z 구간별로 투영한다.

- 일반 레이어의 실제 extrusion 높이 구간과 겹치는 트리 계산 레이어에만 넣는다.
- 간격은 `tree branch radius + normal extrusion width / 2 + SCALED_EPSILON`을 기준으로 한다.
- 모델용 `support_object_xy_distance`를 두 채널 사이에 다시 적용하지 않는다.
- Organic은 sparse normal line을 hard obstacle로 주입하면 influence-area solver가 전체 가지를 잃는 경우가 있어 obstacle 주입을 하지 않는다. 대신 같은 Z 병합에서 normal envelope 바깥의 tree centerline만 보존한다.
- Classic은 `TreeSupportData`에 별도 extra-obstacle 필드를 추가하고 collision/avoidance cache key 계산에 포함한다.
- 같은 Z에서 normal이 물리적 겹침의 소유자이며, tree path는 `normal envelope + tree path 반폭`으로 clip되어 중복 압출 없이 맞닿는다.
- obstacle로 인해 tree component 전체가 경로를 잃으면 조용히 누락하지 않고 진단 오류를 기록한다. 1차 정책은 해당 component를 normal로 재배정하지 않고 실패로 보고한다. 자동 재배정은 커버 역치 의미를 깨므로 별도 기능으로 미룬다.

### 10.8 단일 raft 계약

두 채널이 raft를 각각 출력하면 중복 extrusion이 생긴다. 현재 구현은 normal raft를 소유자로 삼고 같은 Z tree raft의 실제 path를 재귀적으로 평탄화한 뒤, normal 압출 envelope와 겹치지 않는 잔여 구간만 붙인다. `ExtrusionEntityCollection`, `ExtrusionPath`, `ExtrusionMultiPath`, `ExtrusionLoop`를 명시적으로 처리하며 알 수 없는 entity는 오류로 중단한다.

### 10.9 같은 Z 레이어 병합 계약

`merge_support_layer_channels()`만 최종 병합을 담당한다.

- Z 비교: 기존 `EPSILON` 규칙;
- 정렬: `print_z` 오름차순, 최종 ID 연속 재부여;
- height: 같은 Z 그룹의 최소 height;
- extrusion: 소유권 이동 append, 복사 금지;
- `support_islands`, `lslices`, `base_areas`: 각각 합집합;
- channel type: normal only, tree only, mixed 중 하나;
- `interface_id`: path 생성이 끝난 뒤에는 방향 계산에 쓰지 않으므로 대표값을 저장하되 debug에서 불일치를 기록;
- classic tree `area_groups`: toolpath 생성 완료 후 사용 종료를 확인하고 최종 레이어에는 보존하지 않는다. 포인터가 이동된 polygon을 가리키게 두면 안 된다.

병합 뒤 검증 함수가 다음 불변식을 검사한다.

- Z 정렬 및 동일 Z 중복 없음;
- 모든 extrusion entity 소유자가 정확히 하나;
- mixed layer는 두 channel footprint를 모두 포함;
- support island와 base area가 비어 있는데 해당 역할 extrusion이 존재하는 비정상 상태 없음.

## 11. 패치 단위와 각 단계의 중단 조건

### Patch 0: 안전 기준점

- 현재 HEAD에 로컬 backup branch 생성;
- 사용자 미추적 release note는 건드리지 않음;
- Mixed 관련 변경만 diff에 들어가는지 확인.

중단 조건: 작업 트리에 Mixed와 겹치는 사용자 변경이 발견되면 구현 전에 보고한다.

### Patch 1: 설정과 UI만 추가

- enum map, 기본값, 범위, tooltip, UI 노출 조건;
- legacy `hybrid(auto)` migration 유지;
- invalidation과 preset option 목록 갱신;
- serialization 단위 테스트 추가.

중단 조건: `mixed(auto)` round-trip 또는 기존 `hybrid(auto)` migration이 명확하지 않으면 다음 단계로 가지 않는다.

### Patch 2: 수요 검출 API 분리

- Prusa/Cura 수요 검출을 side-effect 최소 API로 노출;
- 자동/강제 수요 분리 유지;
- 기존 일반 모드가 새 API를 호출하도록 바꾸되 결과 동등성 테스트 추가.

중단 조건: 기존 일반 모드의 contact polygon 결과가 바뀌면 원인을 확정하기 전 planner를 붙이지 않는다.

### Patch 3: planner와 순수 기하 테스트

- buildplate shadow, R-tree 후보 탐색, DSU, coverage 계산;
- 0/80/100 및 epsilon 경계 테스트;
- 두 클러스터를 뒤늦게 잇는 bridge polygon 회귀 테스트;
- 입력 순서를 섞어도 component와 배정 결과가 같은지 테스트.

중단 조건: 순서 독립성 또는 exact-threshold 테스트 실패.

### Patch 4: 실행 문맥과 feature-off 동등성

- `SupportGenerationContext`를 공통/네 생성 경로에 전달;
- 저장 config 재조회 제거;
- 기존 모드용 기본 overload 유지;
- 기존 Prusa/Cura/Tree 고정 fixture의 support geometry hash 비교.

중단 조건: Mixed를 선택하지 않은 출력이 바뀌면 Mixed dispatch를 구현하지 않는다.

### Patch 5: 필터된 단일 채널 실행

- normal mask만, tree mask만 각각 독립 실행;
- Organic과 classic 모두 독자 detector 우회;
- 빈 mask fast path.

중단 조건: 생성기가 mask 밖 contact를 만들거나 mask 안 수요를 누락.

### Patch 6: obstacle field와 격리 컨테이너

- normal 결과를 Z-aware obstacle로 변환;
- Organic/Classic routing에 주입;
- RAII take/install API 추가;
- 취소/예외 소유권 테스트.

중단 조건: 채널 간 extrusion envelope 중첩 또는 임시 레이어 누수.

### Patch 7: 단일 raft와 최종 병합

- 채널 raft emission 차단;
- `MixedRaftPlan` 한 번 생성;
- 같은 Z 병합과 `stInnerMixed` 설치.

중단 조건: raft가 두 번 출력되거나 tree root/normal column 중 하나가 raft footprint 밖에 존재.

### Patch 8: 모든 소비처 갱신

- `Brim.cpp`, `Print.cpp`, `GCode.cpp`, cache JSON, arrange, preview, gizmo, validation;
- exact enum 비교를 channel 질의로 교체;
- mixed first layer에 일반과 트리 footprint가 모두 반영되는지 확인.

중단 조건: `stInnerNormal/stInnerTree` exact 비교가 의도 검토 없이 남아 있음.

### Patch 9: 전체 검증

- 8개 생성 조합, 역치, 노즐, raft, 이종 재질, blocker/enforcer;
- G-code 경계와 중복 extrusion 검사;
- 성능 계측과 feature-off 회귀 비교.

이 단계의 실제 빌드 및 테스트는 사용자의 완료 지시에 따라 수행했다.

## 12. 구현 중 반드시 지킬 불변식

1. Selective merge OFF에서는 한 수요 component가 normal과 tree에 동시에 들어가지 않는다. ON에서는 한 component가 공간 분할될 수 있지만 한 XY 점의 소유자는 항상 하나뿐이다.
2. 수요 판정은 한 번만 하며 두 엔진이 다시 판정하지 않는다.
3. 누적 하부 그림자 계산 함수는 하나만 존재한다.
4. 저장된 객체 config는 generation 도중 불변이다.
5. 임시 `SupportLayer*`는 항상 RAII 소유자 한 곳에 속한다.
6. raft path는 한 번만 생성한다.
7. 기존 support type의 dispatch와 serialized value는 바뀌지 않는다.
8. Mixed 설정 변경은 최소 `posSupportMaterial`을 invalidate하며, 수요 검출에 영향을 주는 변경은 기존 slice invalidation 규칙도 보존한다.
9. 출력 레이어는 Z 정렬, 연속 ID, 단일 소유권을 만족한다.
10. 알고리즘 결과는 입력 polygon 순서와 thread scheduling에 무관하다.

## 13. 하면 안 되는 구현

- 새 기능을 기존 `hybrid(auto)` 문자열에 연결하지 않는다.
- 기존 Tree Hybrid의 large-overhang heuristic을 커버율 판정으로 속이지 않는다.
- `is_tree()`에 Mixed를 무조건 포함해 전체 코드를 우회 분기시키지 않는다.
- 실행 중 `PrintObjectConfig::support_type`이나 `support_style`을 임시 변경하지 않는다.
- 두 생성기를 같은 `support_layers()`에 바로 실행하지 않는다.
- 완성된 G-code나 extrusion path를 충돌 검사 없이 이어 붙이지 않는다.
- 레이어마다 normal/tree를 다시 선택해 한 덩어리의 중간에서 타입을 바꾸지 않는다.
- Selective merge에서 normal/tree canonical mask를 의도적으로 겹치게 만들어 접합부를 해결하지 않는다.
- `support_on_build_plate_only` 적용 뒤 남은 영역으로 coverage를 계산하지 않는다.
- 기존 `OverhangCluster`를 그대로 복사하지 않는다.
- 매 component마다 누적 그림자 union을 다시 계산하지 않는다.
- 좌표를 mm float로 바꿔 boolean 연산하거나 면적 판정을 하지 않는다.
- raft를 두 채널에서 각각 생성하지 않는다.
- classic tree의 `area_groups` 포인터를 polygon 이동 뒤 보존하지 않는다.
- Mixed 작업과 Tsunami 삭제, 버전 변경, README, 릴리스 정리를 한 커밋에 섞지 않는다.
- 회귀 원인이 불명확한 상태에서 설치파일 생성이나 push로 넘어가지 않는다.

## 14. 계획 최적화 루프 결과

### Loop 1: 의미 정확성

초안의 레이어별 판정 가능성을 제거하고 3D connected component 단위로 고정했다. exact threshold는 normal로 가는 inclusive 비교로 고정했다.

### Loop 2: 구조 안전성

두 엔진 최종 결과 단순 병합안을 폐기했다. 명시적 실행 문맥, 격리된 레이어 소유권, tree obstacle field, 단일 raft 계약을 추가했다.

### Loop 3: 결정성과 성능

기존 첫 일치 `OverhangCluster` 재사용안을 폐기했다. R-tree 후보 축소 + union-find로 바꾸고, 누적 shadow 1회 계산, bbox fast path, threshold 0 fast path, 빈 채널 생략을 추가했다.

### Loop 4: 호환성

`support_type`을 다시 읽는 48개 계열 호출부와 `SupportInnerType` exact 비교 소비처를 감사했다. Mixed를 기존 tree/normal 중 하나로 위장하지 않고 용도별 helper와 `stInnerMixed`를 쓰도록 계획을 수정했다.

### Loop 5: 실패 복구성

각 patch마다 다음 단계로 넘어가지 않는 중단 조건을 추가했다. 특히 feature-off 동등성 검증을 generator dispatch보다 먼저 배치해 기존 모드 회귀를 초기에 차단한다.

## 15. 성능 예산과 계측

planner 추가 비용 목표:

- 전체 Mixed support 시간의 10% 이하 또는 500 ms 이하 중 큰 값;
- peak memory는 수요 polygon 원본 크기의 3배 이내;
- 같은 모델/설정 반복 실행 시 component 배정 hash 동일.

debug timing을 다음 구간으로 나눈다.

```text
demand_detection
shadow_prefix
component_candidates
component_union
coverage_classification
normal_generation
obstacle_projection
tree_generation
raft_generation
layer_merge
```

성능 예산을 넘으면 순서대로 최적화한다.

1. bbox reject 비율과 불필요한 boolean difference 수 확인;
2. R-tree query 후보 수 확인;
3. polygon 복사 대신 move/reference 수명 정리;
4. 레이어별 coverage difference 병렬화;
5. collision cache에서 obstacle union 반복 여부 제거.

결과가 달라질 수 있는 근사화, polygon 단순화 확대, 임계값 반올림은 성능 최적화 수단으로 사용하지 않는다.

## 16. 구현 완료 판정

다음 항목이 모두 충족돼야 구현 완료다.

- UI와 serialization round-trip;
- Prusa/Cura 수요 기준의 component 배정 테스트;
- 8개 normal/tree 조합에서 두 채널 생성;
- exact threshold와 입력순서 독립성;
- 채널 간 path envelope 중복 없음;
- raft 단일 생성;
- mixed first-layer/brim/retraction/preview/cache 정상;
- 이종 support/interface filament 정상;
- 0.2/0.4/0.6/0.8 mm nozzle G-code 경계 통과;
- 기존 support types의 fixture/hash 회귀 없음;
- 취소/예외 시 누수와 dangling pointer 없음;
- 계획서의 금지 항목 위반 없음.

## 17. 실행 검증 결과

- Release `fff_print_tests` 타깃 빌드 성공.
- `[Mixed]`: 9 test cases, 486 assertions 통과.
- 실제 선택적 분할 fixture에서 Prusa/Cura × Organic/Slim/Strong/Tree Hybrid 8개 조합의 support layer와 G-code 생성 통과.
- Prusa+Organic 및 Cura+Strong 대표 조합에서 2-layer raft 설정, 부분 겹침 raft 병합, 유일한 Z 순서, 각 raft 레이어 extrusion, G-code 생성 통과.
- 0.2/0.4/0.6/0.8 mm nozzle × Organic/Slim/Strong/Tree Hybrid 16개 조합에서 양 채널 support layer와 G-code 생성, 112 assertions 통과.
- `[SupportMaterial]~[TsunamiSupport]`: 기존 Normal/Cura/Tree/인터페이스/raft/온도 드롭 타워 포함 33 test cases, 5,840 assertions 통과.
- Release 애플리케이션 타깃 `OrcaSlicer` 전체 빌드 및 `MagpieSlicer.dll` 링크 성공.
- 전체 `[SupportMaterial]` 실행에서 기존 Tsunami 전용 테스트 실패가 재현되어 해당 폐기 예정 기능은 Mixed 회귀 판정에서 제외했다. Mixed가 아닌 기존 실패이며 이 작업에서 수정하지 않았다.

## 18. Mixed 서포트 페인팅 설계와 구현

Mixed에서 수동 페인트는 기존 support enforcer/blocker 데이터에 생성기 종류를 억지로 인코딩하지 않고 두 annotation으로 분리한다.

- `supported_facets`: 기존 의미를 그대로 유지한다. ENFORCER는 해당 면에 서포트 수요를 강제하고 BLOCKER는 서포트를 막는다.
- `mixed_support_facets`: Mixed 전용 생성기 배정만 저장한다. NONE은 자동 판정, ENFORCER는 일반, BLOCKER는 트리를 뜻한다.
- 이전 버전은 새 attribute를 무시하더라도 `supported_facets`의 ENFORCER를 읽으므로 수동 서포트 수요 자체는 사라지지 않는다.
- Mixed가 아닌 모드의 페인터와 생성기는 기존 경로를 그대로 사용한다. 숨은 channel annotation은 실제 `supported_facets` ENFORCER 투영과 교집합을 취한 뒤에만 planner에 들어가므로 stale 데이터가 자동 overhang 배정을 덮어쓰지 않는다.

Mixed 페인터 UI는 다음 상태를 사용한다.

- 미도색: 자동 component 판정;
- 초록: 일반 서포트 강제;
- 파랑: 트리 서포트 강제;
- 빨강: 서포트 차단;
- 노랑: 새 채널 값이 없는 기존 generic enforcer. 서포트는 강제하지만 생성기 선택은 자동이다.

입력은 기존 페인터 제스처를 보존한다. 좌클릭은 UI에서 선택한 일반/트리 채널을 칠하고, 우클릭은 차단하며, Shift+좌클릭은 자동 상태로 지운다. 일반으로 칠한 투영 중 build plate에서 수직으로 도달할 수 없는 부분은 누락시키지 않고 트리 채널로 폴백하며 non-critical warning을 남긴다. 투영 중 일반과 트리가 겹치면 build-plate-only 불변식을 지키기 위해 트리가 우선한다. 전체 우선순위는 blocker > tree > normal > automatic이다.

편집 시 화면 내부에서는 자동/기존 강제/차단/일반/트리의 5상태 selector로 합치고, 저장할 때 두 annotation으로 다시 분리한다. 동일 topology의 두 annotation은 상태만 직접 overlay하여 반복해서 페인터를 열고 저장해도 삼각형 분할 수가 늘거나 경계가 이동하지 않게 했다. topology가 다른 외부/과거 데이터에만 기하 overlay fallback을 사용한다.

영속 경로는 standard 3MF의 `slic3rpe:mixed_supports`와 Bambu/Orca 3MF의 `paint_mixed_supports`를 사용한다. volume copy, unit conversion, split/remap, reload/repair, undo/redo, background invalidation과 shared-mesh 판정에도 별도 annotation을 포함한다.

검증 결과:

- `libslic3r_tests [Mixed]`: selector 합성/분리, standard 3MF와 Bambu/Orca 3MF round-trip을 포함해 3 cases, 28 assertions 통과;
- `fff_print_tests [Mixed]`: 수동 normal/tree 우선순위, projected overlap에서 tree 우선, 도달 불가 normal의 tree 폴백을 포함해 9 cases, 486 assertions 통과;
- Release `libslic3r`, `libslic3r_gui`, 두 테스트 타깃 빌드 성공.
