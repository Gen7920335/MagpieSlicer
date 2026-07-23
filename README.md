# OrcaProject

[![Release](https://img.shields.io/github/v/release/Gen7920335/OrcaSlicer?display_name=tag&sort=date)](https://github.com/Gen7920335/OrcaSlicer/releases/latest)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![Status](https://img.shields.io/badge/status-experimental-orange)
[![Upstream](https://img.shields.io/badge/upstream-OrcaSlicer-2F80ED)](https://github.com/OrcaSlicer/OrcaSlicer)

> Cura 스타일 서포트, 삼각형 인터페이스, 멀티 노즐 출력, 싱글 노즐 저온
> 인터페이스를 개발하는 OrcaSlicer 기반 Windows 포크입니다.

- Cura 스타일 일반 서포트 생성
- 삼각형 서포트 인터페이스와 인터페이스 서브레이어
- 서로 다른 노즐 구경을 사용하는 멀티 툴헤드 출력
- 싱글 노즐 저온 서포트 인터페이스

이 저장소는 실험 버전입니다. 슬라이싱 결과는 검증 중이며 실제 장비 출력은 사용자가
G-code와 툴체인지 동작을 확인한 뒤 진행해야 합니다.

원본 프로젝트: [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer)

**바로가기:**
[다운로드](#다운로드) ·
[현재 상태](#현재-상태) ·
[변경 사항](#변경-사항) ·
[검증 현황](#검증-현황) ·
[제한 사항](#알려진-제한-사항) ·
[향후 작업](#향후-작업) ·
[빌드](#빌드)

## 다운로드

**[OrcaSlicer TrInterface Cura Support 2.5.0-dev 다운로드](https://github.com/Gen7920335/OrcaSlicer/releases/tag/trinterface-cura-support-v2.5.0-dev-20260723)**

- 운영체제: Windows x64
- 형식: NSIS 설치 프로그램
- 파일: `OrcaSlicerTrInterface_Windows_Installer_V2.5.0-dev_x64.exe`
- SHA-256: `A62D49E24F29BBF9A86F374D2B7267E54CE1F06DB773C1F2FCBE5BEFE0BB5C45`
- 앱 이름: `OrcaSlicer TrInterface`
- 실행 파일: `orca-slicer-trinterface.exe`
- 설치 식별자: `OrcaSlicerTrInterface`

원본 OrcaSlicer와 이름, 실행 파일, 설치 레지스트리 키가 분리되어 동시에 설치할 수 있습니다.
포터블 배포는 중단했으며 현재 배포 기준은 설치 프로그램입니다.

## 현재 상태

**상태 기준:** `검증`은 자동 테스트 또는 CLI 슬라이싱 통과를 뜻합니다.
`장비 검증 필요`는 실제 프린터 출력이 아직 완료되지 않았다는 뜻입니다.

| 영역 | 상태 | 설명 |
| --- | --- | --- |
| Cura 스타일 일반 서포트 | 구현·CLI 검증 | 별도 일반 서포트 유형, 영역 처리 및 Cura식 ZigZag 연결 |
| 삼각형 인터페이스 | 구현·단위 테스트 | 120도 간격의 세 방향 인터페이스 패턴 |
| 인터페이스 서브레이어 | 구현 | 범위, 패턴, 각도, 온도 분리 |
| 자동 트리 임계각 | 수정됨·추가 검증 필요 | 자동 트리에서 임계각 설정 전달 경로 수정 |
| 저온 인터페이스 | 구현·장비 검증 필요 | 온도 전환, 보조 냉각, 브러시, 온도 드롭 타워 |
| 멀티 노즐 외벽 | 구현·회귀 검증 중 | Classic/Arachne, 소구경 외벽, 벽수, 겹침, 인터락킹 |
| 대구경 오버라이드 | 구현·회귀 검증 중 | 레이어 범위별 핫엔드 강제 선택 |
| G-code 노즐 보기 | 구현 | `Nozzle used` 보기와 노즐별 고정 색상 |
| 프로젝트 설정 저장 | 구현·테스트 추가 | 커스텀 멀티 노즐 설정을 프로젝트 설정으로 복원 |
| LESIC 캘리브레이션 | 현재 브랜치 미통합 | `lowtempinterface_single` 브랜치에만 존재 |
| 설치 프로그램 | 생성·실제 설치 검증 | 설치본 실행 파일로 Cura 서포트 3개 케이스 통과 |
| 실제 프린터 출력 | 미검증 | 장비별 툴체인지, 냉각, 와이핑 검증 필요 |

## 변경 사항

아래 항목을 펼치면 구현 방식과 설정을 볼 수 있습니다.

<details>
<summary><strong>1. Cura 스타일 일반 서포트</strong></summary>

서포트 유형에 Orca/Prusa 방식과 구분되는 Cura 스타일 일반 서포트를 추가했습니다.

- `Normal (Cura style) auto`: 임계각을 사용하는 자동 일반 서포트
- `Normal (Cura style)`: 수동·강제 영역을 포함하는 일반 서포트
- 트리·오가닉 서포트는 Cura 스타일 일반 서포트의 적용 대상이 아닙니다.
- 오버행 영역을 레이어 방향으로 전파하고 완화하는 전용 경로가
  `Support/CuraStyleSupport.cpp`에 분리되어 있습니다.
- 일반 서포트 본체의 ZigZag는 Cura의 `connectLines()` 구조를 Orca의 Fill 파이프라인에
  맞게 이식했습니다.
- 서포트 영역에 구멍이 있으면 억지 중앙 연결선을 추가하지 않고 독립된 연속 경로를
  유지합니다.
- 결과를 잘라내는 후처리로 모양을 숨기지 않고 서포트 영역 생성과 경로 연결 단계에서
  형상을 결정합니다.
- 기존 Orca/Prusa 일반 서포트와 트리 서포트 경로는 별도 선택지로 유지합니다.

현재 구현은 CuraEngine 전체를 그대로 포함한 것이 아닙니다. Cura의 일반 서포트 영역 처리와
ZigZag 연결 방식 중 필요한 부분을 OrcaSlicer의 폴리곤·레이어·Fill 구조에 맞게 옮긴
네이티브 구현입니다.

</details>

<details>
<summary><strong>2. 삼각형 서포트 인터페이스</strong></summary>

서포트 인터페이스 패턴에 `Triangles`를 추가했습니다.

- 선 방향은 기준 각도를 포함한 세 방향으로 제한됩니다.
- 각 방향은 120도 간격입니다.
- 일반 서포트와 트리 서포트의 인터페이스·접촉면 경로에서 사용할 수 있습니다.
- 인터페이스 간격 설정을 기존 인터페이스 패턴과 같은 설정 경로로 전달합니다.
- 삼각형 경로가 임의 방향으로 새지 않도록 지원 인터페이스용 고정 방향 검사를 추가했습니다.
- `Triangle support-interface fill keeps three fixed directions` 단위 테스트가 포함되어 있습니다.

추가로 실제 곡면 하부에서의 표면 평활도, 간격 0에서의 완전 밀집, 얇은 섬의 경로 연속성은
계속 검증해야 합니다.

</details>

<details>
<summary><strong>3. 인터페이스 서브레이어</strong></summary>

모델과 직접 닿는 인터페이스 레이어를 1번으로 보고, 지정 범위만 별도 패턴으로 출력할 수
있습니다.

| GUI 설정 | 내부 설정 키 | 동작 |
| --- | --- | --- |
| Interface sublayer pattern | `support_interface_sublayer_pattern` | 서브레이어 기능 켜기/끄기 |
| Interface sublayer start | `support_interface_sublayer_start_layer` | 시작 레이어, 최솟값 2 |
| Interface sublayer end | `support_interface_sublayer_end_layer` | 종료 레이어, 총 인터페이스 수로 자동 제한 |
| Interface sublayer pattern type | `support_interface_sublayer_pattern_type` | 패턴 선택 |
| Interface sublayer angle | `support_interface_sublayer_angle` | 0~180도 |
| Interface sublayer temperature | `support_interface_sublayer_temperature` | 0이면 별도 온도 제어 안 함 |

지원 패턴은 `Default`, `Rectilinear`, `Concentric`, `Rectilinear Interlaced`, `Grid`,
`Triangles`입니다. 시작과 종료 범위가 중간 레이어만 포함해도 처리할 수 있으며, 총
인터페이스 레이어가 1 이하이면 사용할 수 없습니다.

</details>

<details>
<summary><strong>4. 자동 트리 임계각</strong></summary>

자동 트리 서포트에서 임계각이 UI에만 존재하고 실제 생성 경로에 전달되지 않던 부분을
수정했습니다.

- 자동 트리에서도 임계각 설정을 사용합니다.
- 강제 서포트 페인팅과 자동 임계각 판정을 분리합니다.
- 일반 Cura 스타일 서포트 선택과 트리 선택이 서로의 생성 경로를 바꾸지 않도록
  `SupportType` 분기를 분리했습니다.

복잡한 곡면과 70도·90도 임계각에 대한 CLI 회귀 케이스는 유지하고 있으나, 모든 프린터
프로필 조합에 대한 GUI 검증은 남아 있습니다.

</details>

<details>
<summary><strong>5. 싱글 노즐 저온 서포트 인터페이스</strong></summary>

한 노즐에서 모델과 서포트 인터페이스의 압출 온도를 분리하기 위한 기능입니다.

기본 출력 순서는 다음과 같습니다.

`모델 → 서포트 본체 → 냉각/온도 전환 → 인터페이스 → 재가열 → 다음 레이어`

| GUI 설정 | 내부 설정 키 | 동작 |
| --- | --- | --- |
| Low temperature support interface | `single_nozzle_low_temperature_interface` | 전체 기능 켜기/끄기 |
| Support interface temperature | `support_interface_temperature` | 인터페이스 압출 온도 |
| Interface exit heating time | `support_interface_heating_time` | 모델 복귀 전 추가 가열 시간 |
| Use auxiliary fan during hotend temperature changes | `support_interface_auxiliary_fan_cooling_on_temperature_change` | 노즐 냉각용 보조 팬 사용, 유로 어댑터 필요 |
| Auxiliary fan speed | `support_interface_auxiliary_fan_speed` | 냉각 중 보조 팬 속도 |
| Wipe nozzle after hotend temperature changes | `support_interface_nozzle_wiping_on_temperature_change` | 유효한 브러시 좌표가 있을 때 노즐 와이핑 |
| Use temperature drop tower | `support_interface_temperature_drop_tower` | 브러시를 사용하지 않을 때 온도 드롭 타워 사용 |

온도 드롭 타워:

- 베드 좌측 후방에서 출력 가능한 위치를 찾습니다.
- ㄱ자 형태의 4줄 경로를 사용합니다.
- 온도 차이가 30도 이하면 기본 크기 50 mm입니다.
- 30도를 초과하면 1도당 1 mm씩 커집니다.
- 최대 크기는 80 mm입니다.
- 일반 타워 경로는 30 mm/s, 냉각 전환 경로는 10 mm/s입니다.
- 레이어 단위 출력에서만 동작하며 객체별 출력에서는 비활성화됩니다.
- 유효한 노즐 브러시 설정이 있으면 드롭 타워보다 브러시를 우선합니다.

보조 팬 냉각과 노즐 와이핑은 해당 장치 설정이 없으면 실제 G-code 동작을 만들지 않습니다.
다만 장비별 냉각 위치·브러시 좌표 데이터베이스는 아직 완성되지 않았습니다.

</details>

<details>
<summary><strong>6. 멀티 노즐 툴헤드</strong></summary>

재료 슬롯을 단순 필라멘트 슬롯으로만 취급하지 않고 툴헤드와 재료를 함께 설정하도록
확장했습니다.

- 설정 창 제목은 `Toolhead / Material settings`입니다.
- 각 슬롯에 `Hotend` 페이지가 있습니다.
- 오브젝트·파트의 기존 `extruder` 저장 키는 프로젝트 호환성을 위해 유지하면서 GUI에서는
  `Hotend`로 표시합니다.
- 최대 16개 핫엔드 선택을 고려한 드롭다운을 사용합니다.
- 기기 화면의 중복 노즐 직경 패널은 숨기고 툴헤드 설정을 기준으로 사용합니다.

각 핫엔드에서 다음 값을 mm 단위로 설정할 수 있습니다.

- Nozzle diameter
- Default line width
- Initial layer line width
- Outer wall line width
- Inner wall line width
- Top surface line width
- Sparse infill line width
- Internal solid infill line width
- Support line width
- Bridge line width

노즐 직경을 바꾸면 해당 직경에 맞는 기본 선폭을 계산하며, 사용자가 각 선폭을 다시 수정할
수 있습니다. 노즐·선폭 값은 프린터 및 프로젝트 설정 복원 경로에 포함됩니다.

</details>

<details>
<summary><strong>7. 작은 노즐 크리스프 코너</strong></summary>

큰 노즐이 외곽 루프를 완전히 채울 수 없는 날카로운 모서리와 작은 디테일을 더 작은 노즐로
출력합니다.

| GUI 설정 | 내부 설정 키 | 기본값 |
| --- | --- | --- |
| Use smaller nozzles in crisp corners | `use_smaller_nozzles_in_crisp_corners` | Off |
| Crisp corner toolhead | `crisp_corner_detail_toolhead` | Auto |
| Small nozzle wall count | `crisp_corner_small_nozzle_wall_count` | 0 |
| Small/large nozzle wall overlap | `crisp_corner_nozzle_wall_overlap` | 15% |
| Multi-nozzle interlocking layers | `crisp_corner_interlace_small_nozzle_walls` | Off |

선택 규칙:

- 현재 기본 노즐보다 작은 유효 노즐을 자동 탐색합니다.
- 같은 재료 색상 후보를 우선하고, 없으면 지정한 툴헤드를 사용합니다.
- 두 번째 툴이 더 큰 노즐이면 첫 번째 툴을 소구경 툴로 역선택할 수 있습니다.
- 큰 노즐로 채우지 못하는 부분이 외곽 루프에 있으면 노즐 전환 심을 루프 중간에 만들지 않고
  해당 외곽 루프 전체를 작은 노즐로 배정합니다.
- `Small nozzle wall count`는 기존 큰 노즐 벽 수를 대체하지 않고 바깥쪽에 추가되는 작은
  노즐 벽 수입니다.
- 공간이 부족하면 작은 노즐 외벽을 우선하고 내부 큰 노즐 벽 수를 줄일 수 있습니다.
- Classic과 Arachne 벽 생성 경로를 모두 지원합니다.

벽 결합:

- 작은 노즐 벽과 큰 노즐 벽 사이에 설정된 물리적 겹침을 적용합니다.
- 인터락킹을 켜면 인접 레이어마다 작은 노즐과 큰 노즐의 경계를 한 벽씩 교대로 이동합니다.
- 예를 들어 큰 노즐 3벽, 작은 노즐 4벽이면 `대대대소소소소`와
  `대대대대소소소`가 레이어마다 반복됩니다.
- 벽과 인필의 경계만 교차시키고 인필 패턴 자체는 레이어마다 이동시키지 않습니다.

관련 테스트에는 Classic/Arachne 소구경 라우팅, 두 번째 노즐 크기 역전, 인접 레이어 벽 수,
비활성화 시 기존 툴 라우팅 유지가 포함됩니다.

</details>

<details>
<summary><strong>8. 대구경 핫엔드 오버라이드</strong></summary>

지정한 레이어 범위의 모든 벽을 선택한 핫엔드로 강제 출력할 수 있습니다.

- 시작 레이어와 종료 레이어를 좌우 입력란으로 지정합니다.
- 각 범위에 사용할 핫엔드를 드롭다운으로 선택합니다.
- `Add region`으로 범위를 추가할 수 있습니다.
- 레이어 번호는 1부터 시작하며 양 끝을 포함합니다.
- 범위가 겹쳐도 오류를 내지 않고 마지막으로 일치한 범위를 사용합니다.
- 직렬화 형식은 프로젝트 설정에 저장됩니다.
- 오버라이드가 적용된 레이어에서는 작은 노즐 자동 판정보다 지정 핫엔드가 우선합니다.

</details>

<details>
<summary><strong>9. G-code 미리보기</strong></summary>

슬라이싱 결과에 `Nozzle used` 보기 모드를 추가했습니다.

- `Layer width`와 별도로 실제 사용 노즐을 구분합니다.
- 필라멘트 색상 대신 노즐마다 고정된 서로 다른 색을 사용합니다.
- 범례 폭을 고정해 항목 문자열 길이에 따라 우측 패널이 흔들리지 않도록 했습니다.
- 기존 `Layer height`, `Line width`, `Filament` 보기는 유지합니다.

</details>

<details>
<summary><strong>10. 프로젝트 호환성과 안정성</strong></summary>

- 기존 오브젝트별 `extruder` 키를 유지해 OrcaSlicer의 툴 정렬과 프로젝트 구조를
  재사용합니다.
- 멀티 노즐 커스텀 설정을 프로젝트 설정 오버레이에 포함합니다.
- 오래된 프로젝트에 일부 설정이 없어도 기본값으로 복원합니다.
- 열린 선택형 드롭다운에 잘못된 텍스트가 들어오면 유효한 툴 번호로 정규화합니다.
- 작은 노즐 기능이 꺼져 있으면 기존 단일 노즐 툴 라우팅을 변경하지 않습니다.
- 멀티 노즐 설정이 일반 서포트 파이프라인을 변경하지 않는 회귀 테스트를 추가했습니다.

</details>

<details>
<summary><strong>11. LESIC 캘리브레이션</strong></summary>

LESIC 캘리브레이션 모델은 현재 통합 브랜치에 포함되어 있지 않습니다.
별도 `lowtempinterface_single` 브랜치의 `Add LESIC calibration model` 커밋에 존재합니다.

해당 브랜치에 구현된 범위:

- 원통형 캘리브레이션 모델
- 베드 유효 크기보다 20 mm 작은 직경으로 자동 배치
- 베드 중앙 정렬
- 바닥 숫자·눈금·외곽선
- 작은 베드에서 글자 크기와 굵기 조절
- 글자를 침범하지 않고 원통 바깥으로 나가지 않는 내부 브림

통합 브랜치에 합칠 때 현재 서포트·멀티 노즐 변경을 덮어쓰지 않도록 별도 이식과 회귀 검증이
필요합니다.

</details>

## 검증 현황

최신 설치 프로그램 검증:

- NSIS 설치 프로그램 생성 성공
- 비관리자 셸에서 UAC 승인 후 실제 설치 성공
- 실행 파일, `OrcaSlicer.dll`, 리소스 포함 여부 확인
- 설치 파일 15,086개 확인
- 설치된 실행 파일로 Cura 서포트 형상 3개 CLI 슬라이싱
- `curved`, `stepped`, `narrow` 케이스 3/3 통과
- 생성 G-code 3개가 비어 있지 않은지 확인

추가 로컬 검증:

- 삼각형 인터페이스 세 방향 단위 테스트
- Classic/Arachne 멀티 노즐 라우팅 테스트
- 인접 레이어 인터락킹 벽 수 테스트
- 프로젝트 설정 복원 테스트
- Cura 스타일과 기존 Orca/Prusa 스타일 선택 분리 검사
- 일반/트리 70도·90도 임계각 회귀 케이스
- 버니 단일 레이어에서 동일 레이어 교차 격자 0개 확인

검증 스크립트는 `scripts/`에 있습니다. 분석용 렌더 이미지는 실제 OrcaSlicer GUI가 아니라
G-code 역할과 압출 좌표를 읽어 생성합니다.

## 알려진 제한 사항

- 실제 Snapmaker U1 및 다른 멀티 툴헤드 장비 출력은 아직 완료되지 않았습니다.
- 장비별 툴체인지·퍼지·프라임·와이핑 G-code가 제조사 기본 동작과 일치하는지 확인해야 합니다.
- 장비별 보조 팬 냉각 위치와 브러시 좌표 데이터베이스가 완성되지 않았습니다.
- 유로 어댑터가 없는 보조 팬은 노즐 냉각 기능으로 사용하면 안 됩니다.
- 온도 드롭 타워는 객체별 출력에서 지원하지 않습니다.
- CuraEngine 전체 포트가 아니므로 모든 Cura 설정과 결과가 1:1로 같지는 않습니다.
- 복잡한 구멍·다중 섬·매우 얇은 서포트 영역은 추가 경로 연속성 검증이 필요합니다.
- 작은 노즐 외벽은 극단적으로 좁은 공간에서 요청한 벽 수보다 적게 생성될 수 있습니다.
- Arachne 가변 선폭과 큰 노즐 내부 벽의 경계는 복잡한 모델에서 추가 검증이 필요합니다.
- 다른 PC의 기존 OrcaSlicer 사용자 설정을 자동 마이그레이션하는 기능은 완성되지 않았습니다.
- 설치 프로그램은 코드 서명되지 않았습니다.
- GitHub의 `Publish to WinGet` 워크플로는 포크에 WinGet 토큰이 없어 실패합니다. Release와
  설치파일 업로드에는 영향이 없습니다.

## 향후 작업

완료하지 않은 작업만 표시합니다. 세부 목록은 우선순위별로 펼쳐볼 수 있습니다.

<details open>
<summary><strong>우선순위 0: 실제 출력 전 필수</strong></summary>

- [ ] Snapmaker U1에서 0.4 mm + 0.15/0.2 mm 툴체인지 G-code 검증
- [ ] 툴체인지 전후 퍼지, 프라임, 리트랙션, 온도, 좌표 범위 확인
- [ ] 작은 노즐 벽과 큰 노즐 벽의 접합 강도 출력 시험
- [ ] 인터락킹 인접 두 레이어의 실제 단면 확인
- [ ] 큰 노즐 벽 수, 작은 노즐 벽 수, 인필 결합부를 최소·최대 설정에서 반복 검증
- [ ] Classic과 Arachne에서 동일 기능 조합 비교
- [ ] 대구경 오버라이드 범위, 중첩 범위, 프로젝트 저장·재열기 검증
- [ ] 원본 OrcaSlicer 기능 회귀 테스트 실행

</details>

<details>
<summary><strong>우선순위 1: 멀티 노즐 완성</strong></summary>

- [ ] 큰 노즐이 채우지 못하는 외곽 루프 판정을 복잡한 글자·필렛·양각·음각 모델로 확장
- [ ] 작은 노즐이 불필요한 바닥·상단 전체 면으로 퍼지지 않도록 피처 판정 정밀화
- [ ] 작은 노즐 벽 수와 기존 큰 노즐 벽 수를 모든 얇은 벽 조건에서 독립적으로 보존
- [ ] 소구경/대구경 중심선 간격과 실제 선폭 기반 겹침 계산 정리
- [ ] Arachne 가변 선폭에서 끊기지 않는 폐루프 우선 생성 강화
- [ ] 인필 경계는 교차하되 인필 패턴 형상은 고정되는지 추가 검증
- [ ] 3개 이상, 최대 16개 툴헤드 UI와 프로젝트 데이터 검증
- [ ] 툴헤드별 선폭 기본값 표와 노즐 직경 변경 시 재계산 정책 정리
- [ ] 모든 툴 선택 컨트롤을 입력형이 아닌 유효한 선택형으로 통일

</details>

<details>
<summary><strong>우선순위 2: Cura 스타일 서포트 완성</strong></summary>

- [ ] CuraEngine의 오버행 영역 확장·수축·평활 처리와 결과 비교
- [ ] 곡면에서 위에서 보이는 계단식 돌출을 줄이는 영역 경계 정밀화
- [ ] 지지가 필요한 면의 가장자리를 벗어나지 않는지 복잡한 모델로 검증
- [ ] 구멍이 있는 서포트 영역의 다중 연속 ZigZag 경로 최적화
- [ ] 체크무늬처럼 보이는 동일 레이어 교차 경로 자동 회귀 검사
- [ ] 서포트 패턴별 경로 연결과 패턴 방향 검증
- [ ] 자동 70도·90도, 수동 페인팅, 강제 서포트 조합 검증
- [ ] 기존 Orca/Prusa 일반 서포트와 트리 서포트 결과 불변 검사

</details>

<details>
<summary><strong>우선순위 3: 인터페이스 완성</strong></summary>

- [ ] 간격 0에서 삼각형 인터페이스가 완전히 밀집되는지 검증
- [ ] 세 방향 외의 선이 생성되지 않는지 복잡한 섬에서 확인
- [ ] 삼각형이 끊기거나 우둘투둘해지는 작은 영역 처리 개선
- [ ] 모델 하단 곡면을 지지하는 인터페이스 바닥 평활화
- [ ] 인터페이스 상·하단 형상 일치 검사
- [ ] 서브레이어 범위, 패턴, 각도, 온도 조합 테스트
- [ ] 총 인터페이스 레이어 수보다 큰 종료값의 자동 제한 GUI 검증
- [ ] 저온 인터페이스와 서브레이어 온도를 동시에 사용할 때 우선순위 정의

</details>

<details>
<summary><strong>우선순위 4: 저온 인터페이스 장비 지원</strong></summary>

- [ ] 2018년 이후 보조 팬이 확실한 프린터의 냉각 지원 여부 조사
- [ ] 장비별 안전한 좌측 후방 냉각 위치 등록
- [ ] 브러시가 있는 장비의 시작·끝 좌표, 반복 횟수, 속도 등록
- [ ] 제조사 시작/종료 매크로의 기존 노즐 와이핑과 중복되지 않도록 감지
- [ ] 미지원 장비에서 보조 팬·노즐 와이핑 UI를 비활성화하는 capability 체계 완성
- [ ] 온도 드롭 타워와 모델·프라임 타워·브림 충돌 검사
- [ ] 작은 베드에서 50~80 mm 타워를 배치하지 못할 때의 대체 경로
- [ ] 210~220도에서 170도까지 냉각하는 실제 시간과 타워 길이 보정
- [ ] 인터페이스 종료 후 재가열 시간과 첫 모델 압출 품질 검증

</details>

<details>
<summary><strong>우선순위 5: 캘리브레이션·배포</strong></summary>

- [ ] LESIC 기능을 통합 브랜치에 충돌 없이 이식
- [ ] 200 mm 미만 베드와 비직사각형 베드에서 LESIC 형상 검증
- [ ] 기존 OrcaSlicer 설정 가져오기 기능 정리
- [ ] 설치본 업그레이드·제거·재설치 검증
- [ ] 설치 프로그램 코드 서명
- [ ] 포크에서 `Publish to WinGet` 자동 실행 비활성화
- [ ] 버전 번호와 변경 로그 자동화
- [ ] 영어 UI 문자열과 번역 카탈로그 정리

</details>

## 빌드

<details>
<summary><strong>Windows 빌드와 설치 프로그램 생성 방법</strong></summary>

필수 환경:

- Windows 10/11 x64
- Visual Studio 2022 Build Tools
- CMake
- NSIS
- OrcaSlicer 의존성 빌드 완료

Release 실행 파일:

```powershell
cmake --build build --config Release --target OrcaSlicer_app_gui --parallel
```

결과:

```text
build/src/Release/orca-slicer-trinterface.exe
build/src/Release/OrcaSlicer.dll
```

설치 프로그램:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build_installer.ps1
```

NSIS는 긴 Windows 경로에서 프로필 파일을 열지 못할 수 있으므로 패키징 스크립트가
`C:\OrcaPkg` 아래의 짧은 임시 경로를 사용합니다.

설치 프로그램 검증:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/verify_installer.ps1
```

검증은 짧은 격리 경로에 실제 설치한 뒤 설치된 실행 파일로 CLI 슬라이싱을 수행합니다.
관리자 권한이 없는 셸에서는 UAC 승인이 필요합니다.

</details>

## 브랜치

<details>
<summary><strong>개발 브랜치 목록</strong></summary>

- `integrated_trinterface_multi_nozzle`: 현재 통합 개발 브랜치
- `trinterface-test1`: 초기 삼각형 인터페이스 개발 브랜치
- `supportgen-algorithm-cura-test1`: 초기 Cura 스타일 일반 서포트 개발 브랜치
- `lowtempinterface_single`: LESIC과 초기 저온 인터페이스 개발 브랜치

새 개발은 통합 브랜치에서 진행하며, 큰 변경 전에는 `backups/`에 로컬 백업을 남깁니다.

</details>

## 개발 원칙

<details>
<summary><strong>이 저장소에서 적용하는 개발 원칙</strong></summary>

- 임시 후처리로 결과를 숨기지 않고 생성 알고리즘의 책임 경계에서 수정합니다.
- 기존 OrcaSlicer 기능과 설정 키 호환성을 우선합니다.
- 큰 변경은 백업 후 진행합니다.
- 조사, 수정, 검증 단계는 각각 PowerShell 스크립트로 묶습니다.
- 동일 파일과 명령을 불필요하게 반복 조회하지 않습니다.
- GUI 좌표 자동화 대신 소스, 단위 테스트, CLI 슬라이싱, G-code 분석을 사용합니다.
- 빌드 후 변경 파라미터를 여러 값과 경계값에서 검증합니다.
- 실제 장비 검증 전에는 완료로 표시하지 않습니다.

</details>

## 라이선스

이 포크는 OrcaSlicer를 기반으로 하며 원본 프로젝트의 라이선스를 따릅니다.
자세한 내용은 [LICENSE.txt](LICENSE.txt)를 확인하십시오.
