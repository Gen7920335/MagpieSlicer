<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer_192px_transparent.png" width="150">

# Magpie Slicer

OrcaSlicer 위에 기능을 **모드(mod)** 로 얹은 FFF 3D 프린터 슬라이서

[다운로드](../../releases) · [English](README_EN.md) · [모드 저장소 (magpie-mods)](https://github.com/Gen7920335/magpie-mods) · [OrcaSlicer 원래 안내문](README_ORCASLICER.md)

</div>

> 실제 장비로 보내기 전에 미리보기와 생성된 G-code를 꼭 확인하십시오. 일부 기능은 온도, 툴 교체, 보조 타워를 추가합니다.

## 소개

Magpie Slicer는 [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) 나이틀리를 바탕으로 한 슬라이서입니다. Orca의 슬라이스 코드를 직접 고쳐 기능을 넣던 이전 버전(2.5.0.0.x, Beta 7까지)과 달리, Magpie 기능은 앱이 불러오는 네이티브 모드로 들어 있고 하나씩 켜고 끌 수 있습니다.

- **모드를 모두 끄면 Orca와 같은 방식으로 슬라이스합니다.** 출력과 실사용에 문제가 없는 Orca 버그 수정만 항상 적용되며, 그 목록은 [ORCA_PATCHES.md](ORCA_PATCHES.md)에 있습니다. 같은 설정에서 Orca와 G-code를 비교하는 시험(`scripts/magpie/golden`)으로 이를 확인합니다.
- **Orca 설정 옆에 모드 설정:** 각 모드의 설정은 Orca 설정 페이지의 관련 그룹 옆에 함께 표시됩니다. 서포트 생성기는 `서포트 > 유형` 드롭다운 하나에서 고릅니다.
- **OrcaSlicer와 따로 설치:** 설정 폴더(`%APPDATA%\MagpieSlicer`)와 파일 연결을 OrcaSlicer와 공유하지 않으므로 나란히 설치할 수 있습니다. `orcaslicer://` 링크는 그대로 처리합니다.
- **Orca 사용자 프리셋 가져오기:** 처음 실행할 때 OrcaSlicer 데이터 폴더의 사용자 프리셋을 한 번 복사합니다(이미 있는 파일은 덮어쓰지 않음). 파일 > 가져오기 > "OrcaSlicer 사용자 프리셋 가져오기"로 다시 실행할 수 있습니다.
- **이전 버전 설정 이전:** 이전 버전 프로젝트·프리셋을 열면 Magpie 전용 설정이 해당 모드 설정으로 옮겨집니다.
- **업데이트 확인은 수동:** 도움말 > 업데이트 확인에서 GitHub 릴리스를 조회합니다. 시작할 때 자동으로 확인하지 않습니다.
- **한국어 번역:** 앱과 모드 이름·설정이 모두 한국어로 번역되어 있습니다.

버전은 `<Orca 버전>.<Magpie 라인>.<리비전>` 형식입니다. 최신 정식 버전은 2.5.0.2.0, 최신 베타는 [2.5.0.2.1 베타](../../releases/tag/v2.5.0.2.1)입니다. Windows 10/11 x64 설치 파일을 [Releases](../../releases)에서 받을 수 있습니다.

### 2.5.0.2.1 베타에서 바뀐 점

- **설정 화면 범주:** 공정·필라멘트·프린터 탭의 옵션을 범주로 묶어 접고 펼 수 있습니다. 범주 제목에 옵션 수가 보이고, 스크롤하면 현재 범주 제목이 위에 고정됩니다.
- **한국어 번역 완성:** 소스에서 쓰는 모든 문자열과 프린터 연결 패널, 범주 제목까지 번역했습니다.
- **Snapmaker U1 툴 교체 높이:** 툴 교체 매크로의 상대 이동(G91)을 쿨링 단계가 잘못 읽어, 툴 교체 뒤 외벽이 2 mm 허공에 출력되던 문제를 고쳤습니다.
- **H/2 서포트:** 접촉 영역에만 절반 높이 서포트를 쓰는 옵션과 밴드 관련 수정 여러 건.
- **오류·크래시 수정:** 슬라이스 중 "Serializing NaN" 오류, H2D 0.4/0.6 노즐 태그 크래시, Bambu 듀얼 노즐 자동 매핑 후 멈춤, 플레이트 필라멘트 순서·유량 검증 크래시, 트리 서포트 반경 0 오류. 크래시 로그 옆에 미니덤프를 남깁니다.
- **서포트·출력:** 레진 서포트 접점과 기둥 받침, 래프트 위 첫 서포트 레이어, 좁은 섬, 노즐별 프라임 타워 폭, 저온 접점 드롭 타워 레이어 높이.

전체 목록은 [릴리스 노트](../../releases/tag/v2.5.0.2.1)에 있습니다.

## 기본 모드

앱에 번들되는 공식 모드 10개입니다. 소스는 이 저장소의 `mods/`와 [magpie-mods](https://github.com/Gen7920335/magpie-mods)에 있습니다.

| 모드 | 하는 일 | 설정 위치 |
| --- | --- | --- |
| 다중 노즐 벽 (`multi_nozzle_walls`) | 노즐 크기가 다른 툴헤드를 함께 써서 날카로운 외벽은 작은 노즐, 나머지는 큰 노즐로 출력. 디테일 툴 자동/지정, 레이어 구간별 툴헤드 지정 | 품질 > 다중 노즐 벽 |
| 절반 높이 외벽과 서포트 (`half_layer`, H/2) | 바깥 벽 2줄과 서포트를 레이어 높이의 절반으로 두 번 출력. 두 번째 패스는 레이어 시간의 중간에 끼워 냉각을 고르게. 서포트는 접촉 영역에만 적용할 수도 있음 | 품질 > 벽 수 옆, 서포트 옆 |
| Cura 서포트 (`cura_support`) | CuraEngine 방식의 일반(영역) 서포트. 계단·타워·원뿔 등 Cura 설정 | 서포트 > 유형 |
| 혼합 서포트 (`mixed_support`) | 베드에 닿는 곳은 일반, 나머지는 트리 서포트로 결합. 페인팅으로 구역 지정 가능 | 서포트 > 유형 |
| 레진 서포트 (`resin_support`) | PrusaSlicer SLA 지지대 방식을 FFF 서포트로 출력. 모델을 띄우고 점 접촉으로 지지 | 서포트 > 유형 |
| 서포트 추가 설정 (`support_extras`) | 삼각형 접점 패턴, 접점 서브레이어(패턴·각도·온도), 일반·트리 서포트 벽 수 | 서포트 > 접점 패턴 옆 |
| 저온 서포트 접점 (`low_temp_interface`) | 서포트 접점만 낮은 온도로 출력해 모델에 덜 붙게. 제자리 대기 또는 온도 강하 타워, 보조 팬·브러시 | 서포트 > 저온 서포트 접점 |
| 레이어 타임 블렌딩 (`layer_time_blend`) | 레이어마다 다른 출력 시간을 대기로 고르게 맞춤. 대기 위치는 전용 타워·프라임 타워·파킹·제자리 | 속도 > 레이어 타임 블렌딩 |
| 저온 접점 캘리브레이션 (`low_temp_calibration`) | LESIC 링 보정(온도 × 체적 유량)과 원터치 격자 보정(두 설정 조합별 시편) | 캘리브레이션 메뉴 |
| Snapmaker U1 (`snapmaker_u1`) | U1에서 순정 시작 순서(PRINT_START, 홈, 베드 메시, 퍼지)가 빠진 시작 G-code를 보호. 기본 켜짐 | 프린터 > 시작 G-code 옆 |

그 밖에 Snapmaker U1 모니터 플러그인이 들어 있습니다.

### 모드 없이도 달라지는 것

- **노즐별 선폭:** 노즐 크기가 섞인 프린터에서는 절대 선폭(mm)이 기준 노즐 대비 비율로 해석되어 노즐마다 맞게 바뀝니다. 노즐별로 직접 정하려면 품질 > 선폭의 노즐 스위치를 씁니다.
- **툴 보기 팔레트:** 같은 색 필라멘트를 여러 툴헤드가 써도 미리보기 툴 보기에서 툴헤드별로 구분됩니다.
- **설정 정리:** 상위 설정이 꺼지면 하위 설정은 회색이 아니라 숨겨지고, 모드와 Orca의 중복 설정은 하나로 합쳐 보입니다. 설정 탭의 옵션은 접을 수 있는 범주로 묶여 있습니다.

## 모드

### 모드 쓰기

- **설치:** 메인 메뉴 > "모드 받기..."에서 모드 인덱스를 검색해 설치합니다. 수동으로는 패키지 폴더(`<id>/mod.json`)를 `%APPDATA%\MagpieSlicer\mods\`에 복사합니다.
- **관리:** 메인 메뉴 > "설치된 모드..."에서 켜기/끄기/삭제합니다. 변경은 다음 실행부터 적용됩니다.
- **오브젝트별 설정:** 오브젝트나 파트를 우클릭 > "모드 설정..."에서 그 오브젝트에만 다른 값을 줄 수 있습니다(모드가 오브젝트/영역 단위 파라미터를 제공할 때).
- **명령줄:** `--mod-search "검색어"`, `--mod-install id1,id2`, `--mod-index <url>`.
- **출력 확인:** G-code 머리말의 `; mods:` 줄에 그 출력에 실제로 쓰인 모드가 적힙니다.

### 신뢰 등급

모드 패키지는 Ed25519로 서명됩니다. 세 등급의 권한은 같고, 차이는 표시와 경고뿐입니다.

| 등급 | 조건 | 표시 |
| --- | --- | --- |
| 공식 | 설치 파일에 번들되었거나 Magpie 공식 키로 서명 | 파란 배지 |
| 인증 | 제작자가 낸 소스를 검수한 뒤 Magpie 인증 키로 서명 | 파란 배지 "인증" |
| 인증 안 됨 | 서명 없음, 알 수 없는 키, 서명 후 파일 변경 | 빨간 배지, 네이티브 코드 로드 전 처음 한 번 경고 |

모드는 슬라이싱 결과와 프린터가 실행할 G-code를 바꿀 수 있으므로, 인증되지 않은 모드는 믿을 수 있는 곳에서 받은 것만 쓰십시오. 모드가 비정상 종료하면 그 실행 동안 그 모드만 꺼지고 앱은 유지됩니다. 개인 서명 키는 이 저장소에 없습니다.

### 모드 만들기

모드 개발 문서와 SDK는 [magpie-mods](https://github.com/Gen7920335/magpie-mods) 저장소에 있습니다.

- **[모드 개발자 가이드](https://github.com/Gen7920335/magpie-mods/blob/main/docs/index.md):** 신뢰 등급, 모드의 두 종류, 툴체인과 SDK, 패키지 구조, 매니페스트(`mod.json`) 레퍼런스, 네이티브 ABI, 필수 규칙, 다국어, Hello 모드 따라 하기, 테스트, 서명과 배포, 슬롯 상세 레퍼런스, 프린터 연결, 체크리스트, 문제 해결, 공식 모드 예제.
- **SDK** ([`sdk/`](https://github.com/Gen7920335/magpie-mods/tree/main/sdk), 이 저장소의 `mods/sdk`와 같음): C 헤더 `magpie_mod_api.h`와 슬롯별 헤더, C++ 도우미 `magpie_mod.hpp`, CMake 템플릿 `MagpieMod.cmake`. MIT 라이선스라 SDK로 만든 모드는 원하는 라이선스로 배포할 수 있습니다.
- **공식 모드 소스** ([`mods/`](https://github.com/Gen7920335/magpie-mods/tree/main/mods)): 새 모드의 출발점으로 쓸 수 있습니다.

모드는 두 종류입니다.

| 종류 | 내용 |
| --- | --- |
| JSON 모드 | `mod.json` 하나. 설정 페이지에 파라미터를 추가하고 규칙(식)으로 기존 Orca 설정을 바꿉니다. 코드가 없습니다. |
| 네이티브 모드 | `mod.json` + DLL. 코어가 정해 둔 슬롯을 구현해 슬라이싱 동작 자체를 바꿉니다. C ABI라서 앱 소스 없이 SDK만으로 빌드합니다. |

네이티브 모드가 구현할 수 있는 슬롯과 공식 모드의 예:

| 슬롯 | 역할 | 예 |
| --- | --- | --- |
| `wall.plan` | 영역별 벽 계획(벽 수, 노즐 배정) | multi_nozzle_walls |
| `support.areas` / `support.compose` / `support.contacts` | 서포트 생성(영역 / 코어 생성기 조합 / 접점·몸체) | cura_support / mixed_support / resin_support |
| `support.style` | 접점 패턴·서브레이어·벽 수 | support_extras |
| `object.z_offset` | 오브젝트 띄우기 | resin_support |
| `layer.bands` | 레이어 안 물리 밴드 분할 | half_layer |
| `gcode.events` | G-code 내보내기 이벤트(온도·대기·보조 타워·시작 G-code 필터) | low_temp_interface, snapmaker_u1 |
| `gcode.time_plan` | 레이어 시간 대기 계획 | layer_time_blend |
| `calibration.project` | 캘리브레이션 메뉴 항목과 프로젝트 생성 | low_temp_calibration |
| `printer.agent` | 프린터 연결(상태·명령·전송) | — |

코어 서비스 `core.geometry`(폴리곤 연산)와 `core.net`(HTTP/TLS, GUI 전용)을 모드에서 쓸 수 있습니다.

네이티브 모드를 만드는 순서는 다음과 같습니다. 자세한 내용은 가이드 8절(따라 하기)을 보십시오.

1. `sdk/`를 복사하고 `MagpieMod.cmake`로 프로젝트를 구성합니다. 부동소수점 모드는 `precise`여야 하며(템플릿이 설정), 다른 모드로 빌드한 라이브러리는 로드가 거부됩니다.
2. `mod.json`에 id, 버전, API 버전, 바이너리 이름, 구현할 슬롯, 파라미터를 선언합니다. 선언한 슬롯과 실제 등록은 정확히 같아야 합니다.
3. `magpie_mod_query` / `magpie_mod_init` / `magpie_mod_shutdown`을 내보내고 슬롯 테이블을 등록합니다.
4. 패키지 폴더(`<id>/mod.json`, `bin/win-x64/<binary>.dll`, 선택적으로 `i18n/`, `tests/`)를 만듭니다.
5. `magpie-slicer.exe --mod-test <id>`로 검증합니다. 로드, 슬롯 등록, 결정성(같은 입력을 두 프로세스에서 슬라이스해 비교), G-code 안전성을 검사합니다.
6. 배포합니다. 서명 없이 배포하면 "인증 안 됨"으로 로드됩니다. 인증을 원하면 소스와 빌드 방법을 함께 제출해 검수를 받습니다(가이드 10절).

반드시 지킬 규칙(가이드 6절): 코어와는 데이터만 주고받기(C++ 객체·예외 금지), 좌표 스케일은 매 호출 `host->scaling_factor_mm`에서 읽기, 같은 입력엔 같은 출력, 경고·오류는 호스트 API로 보고.

설계 문서: [모드 시스템](docs/HLSD/mods.md), [네이티브 모드](docs/HLSD/native-mods.md), [레이어 타임 블렌딩](docs/HLSD/layer-time-blending.md).

## 빌드 (Windows)

```bat
build_win.bat -s --tests
```

- 테스트: `build` 폴더에서 `ctest -C Release -j8`
- Orca와의 출력 비교: `scripts/magpie/golden/golden.py`
- 무작위 조합 점검: `scripts/magpie/sweep/random_sweep.py`
- 설치파일: `scripts/build_installer.ps1 [-ModSignKey <pem>]`
- 공식 모드 서명: 설치 단계(`cmake --install`)에서 환경 변수 `MAGPIE_MOD_SIGN_KEY`가 서명 키 파일을 가리키면 `resources/mods`에 서명된 모드가 설치됩니다. 없으면 서명 없이 설치됩니다.
- 모드 도구: `--mod-test <id>`, `--mod-sign <폴더> --mod-sign-key <pem>`, `--mod-keygen`

버전 규칙은 [docs/MAGPIE_VERSIONING.md](docs/MAGPIE_VERSIONING.md), Orca 코드에 남은 수정 목록은 [ORCA_PATCHES.md](ORCA_PATCHES.md), 개발 규칙은 [AGENTS.md](AGENTS.md)와 [docs/PROJECT_CONTRACTS.md](docs/PROJECT_CONTRACTS.md)에 있습니다.

## 알려진 제한

- Windows 외 플랫폼은 빌드하지 않았습니다.
- H/2처럼 한 레이어 안에서 툴을 여러 번 바꾸는 기능은 프라임 타워가 커집니다. 배치 오류가 나면 미리보기에서 타워를 옮기거나 자동 배치를 쓰십시오.
- 노즐 크기가 섞인 프린터에서 작은 노즐 기준으로 적은 절대 선폭은 검증 오류가 날 수 있습니다. 비율(%)이나 노즐별 선폭을 쓰십시오.

버그 보고와 제안은 [Issues](../../issues), 보안 관련 보고는 [SECURITY.md](SECURITY.md)를 이용해 주십시오.

## 라이선스와 크레딧

Magpie Slicer는 **GNU Affero General Public License v3.0**([LICENSE.txt](LICENSE.txt))으로 배포됩니다. 이 프로그램을 고쳐 배포하거나 네트워크로 제공하면 그 소스도 같은 라이선스로 공개해야 합니다. 이 프로그램에는 어떠한 보증도 없습니다.

Magpie Slicer는 다음 프로젝트의 작업 위에 있습니다.

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer): SoftFever와 OrcaSlicer 기여자들 (AGPL-3.0). 이 저장소의 대부분의 코드입니다.
- [Bambu Studio](https://github.com/bambulab/BambuStudio): Bambu Lab (AGPL-3.0). OrcaSlicer의 바탕입니다.
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer): Prusa Research (AGPL-3.0). Bambu Studio의 바탕이며, 레진 서포트 모드에 PrusaSlicer 2.9.6의 SLA 지지대 코드가 들어 있습니다.
- [Slic3r](https://github.com/slic3r/Slic3r): Alessandro Ranellucci와 기여자들 (AGPL-3.0).
- [CuraEngine](https://github.com/Ultimaker/CuraEngine): UltiMaker (AGPL-3.0). Cura 서포트 모드가 CuraEngine의 서포트 코드를 옮겨 왔습니다.

각 원본 파일의 저작권 표기는 그대로 유지됩니다. 사용한 라이브러리의 라이선스는 앱의 정보 > 저작권 창에 있습니다. "OrcaSlicer", "Bambu Studio", "PrusaSlicer", "Cura"는 각 소유자의 이름이며, Magpie Slicer는 이들 프로젝트가 만들거나 보증하는 제품이 아닙니다.

모드 SDK의 헤더와 템플릿은 [magpie-mods](https://github.com/Gen7920335/magpie-mods)에서 MIT 라이선스로도 제공되므로, SDK로 만든 모드는 원하는 라이선스로 배포할 수 있습니다.
