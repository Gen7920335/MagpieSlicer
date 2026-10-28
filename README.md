<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer.png" width="150">

# Magpie Slicer Next

OrcaSlicer 위에 기능을 **모드(mod)** 로 얹은 FFF 3D 프린터 슬라이서

[English](README_EN.md) · [모드 저장소 (magpie-mods)](https://github.com/Gen7920335/magpie-mods) · [OrcaSlicer 원래 안내문](README_ORCASLICER.md)

</div>

> 개발 중인 2세대 라인입니다. 실제 장비로 보내기 전에 미리보기와 생성된 G-code를 꼭 확인하십시오.

## 소개

Magpie Slicer Next는 [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) 나이틀리를 바탕으로 한 슬라이서입니다. Orca의 슬라이스 코드를 직접 고쳐 기능을 넣던 1세대(2.5.0.0.x, Beta 7까지)와 달리, Magpie 기능은 앱이 불러오는 네이티브 모드로 들어 있고 하나씩 켜고 끌 수 있습니다.

- **모드를 모두 끄면 Orca와 같은 방식으로 슬라이스합니다.** 출력과 실사용에 문제가 없는 Orca 버그 수정만 항상 적용됩니다. 같은 설정에서 Orca와 G-code를 비교하는 시험(`scripts/magpie/golden`)으로 이를 확인합니다.
- **Orca 설정 옆에 모드 설정:** 각 모드의 설정은 Orca 설정 페이지의 관련 그룹 옆에 함께 표시됩니다.
- **따로 설치:** 앱 키 `MagpieSlicerNext`, 실행 파일 `magpie-slicer-next`, 설치 이름 "Magpie Slicer Next". 설정 폴더와 파일 연결을 OrcaSlicer·1세대와 공유하지 않습니다. `orcaslicer://` 링크는 그대로 처리합니다.
- **Orca 사용자 사전 설정 가져오기:** 처음 실행할 때 OrcaSlicer 데이터 폴더의 사용자 사전 설정을 한 번 복사합니다(이미 있는 파일은 덮어쓰지 않음). 파일 > 가져오기 > "OrcaSlicer 사용자 사전 설정 가져오기"로 다시 실행할 수 있습니다.
- **1세대 설정 이전:** 1세대 프로젝트·사전 설정을 열면 Magpie 전용 설정이 해당 모드 설정으로 옮겨집니다.
- **한국어 번역:** 앱과 모드 이름·설정이 모두 한국어로 번역되어 있습니다.

## 기본 모드

앱에 번들되는 공식 모드입니다. 소스는 이 저장소의 `mods/`와 [magpie-mods](https://github.com/Gen7920335/magpie-mods)에 있습니다.

| 모드 | 하는 일 |
| --- | --- |
| 다중 노즐 벽 (`multi_nozzle_walls`) | 노즐 크기가 다른 툴헤드를 함께 써서 날카로운 외벽은 작은 노즐, 나머지는 큰 노즐로 출력 |
| 절반 높이 외벽과 서포트 (`half_layer`, H/2) | 바깥 벽 2줄과 서포트를 레이어 높이의 절반으로 두 번 출력 |
| Cura 서포트 (`cura_support`) | CuraEngine 방식의 일반(영역) 서포트 |
| 혼합 서포트 (`mixed_support`) | 베드에 닿는 곳은 일반, 나머지는 트리 서포트로 결합 |
| 레진 서포트 (`resin_support`) | PrusaSlicer SLA 지지대 방식을 FFF 서포트로 출력 |
| 서포트 추가 설정 (`support_extras`) | 삼각형 접점 패턴, 접점 서브레이어(패턴·각도·온도), 일반·트리 서포트 벽 수 |
| 저온 서포트 접점 (`low_temp_interface`) | 노즐 하나로 모델과 서포트 접점을 다른 온도로 출력, 온도 하강 타워 포함 |
| 레이어 시간 균일화 (`layer_time_blend`) | 레이어마다 다른 출력 시간을 대기로 고르게 맞춤 |
| LESIC 링 보정 (`lesic`) | 온도·체적 유량을 한 번에 보는 링 캘리브레이션 |
| Snapmaker U1 (`snapmaker_u1`) | U1에서 순정 시작 순서(PRINT_START, 홈, 베드 메시, 퍼지)가 빠진 시작 G-code를 보호 |

그 밖에 Snapmaker U1 모니터 플러그인이 들어 있습니다.

## 모드 시스템

- **네이티브 모드:** 모드는 C ABI로 된 DLL입니다. 코어는 정해진 지점(슬롯: `wall.plan`, `support.areas`, `layer.bands`, `gcode.events` 등)에서 모드에게 계획을 묻고, 모드는 코어의 계산 서비스(예: `core.geometry`)를 빌려 씁니다. 모드 설정은 프로젝트·사전 설정에 저장됩니다. 모드가 충돌하면 그 실행 동안은 그 모드를 다시 부르지 않습니다.
- **서명과 신뢰 단계:** 공식 모드는 Magpie 공식 키로, 인증 모드는 인증 키로 서명됩니다(Ed25519). 서명이 없는 모드도 쓸 수 있지만 "인증 안 됨" 표시와 처음 한 번 경고가 뜹니다. 개인 키는 이 저장소에 없습니다.
- **모드 받기:** 메인 메뉴의 "모드 받기..."에서 모드를 찾아 설치합니다. 명령줄에서는 `--mod-search`, `--mod-install`, `--mod-index`를 씁니다.
- **모드 만들기:** 앱 소스 없이 SDK(`mods/sdk`, magpie-mods의 `sdk/`)만으로 빌드할 수 있습니다. [모드 개발자 가이드](https://github.com/Gen7920335/magpie-mods/blob/main/docs/index.md)를 보세요.

## 빌드 (Windows)

```bat
build_win.bat -s --tests
```

- 테스트: `build` 폴더에서 `ctest -C Release -j8`
- Orca와의 출력 비교: `scripts/magpie/golden/golden.py`
- 무작위 조합 점검: `scripts/magpie/sweep/random_sweep.py`
- 설치파일: `scripts/build_installer.ps1`
- 공식 모드 서명: 설치 단계(`cmake --install`)에서 환경 변수 `MAGPIE_MOD_SIGN_KEY`가 서명 키 파일을 가리키면 `resources/mods`에 서명된 모드가 설치됩니다. 없으면 서명 없이 설치됩니다.

버전 규칙은 [docs/MAGPIE_VERSIONING.md](docs/MAGPIE_VERSIONING.md), Orca 코드에 남은 수정 목록은 [ORCA_PATCHES.md](ORCA_PATCHES.md), 개발 규칙은 [AGENTS.md](AGENTS.md)와 [docs/PROJECT_CONTRACTS.md](docs/PROJECT_CONTRACTS.md)에 있습니다.

## 라이선스와 크레딧

Magpie Slicer Next는 **GNU Affero General Public License v3.0**([LICENSE.txt](LICENSE.txt))으로 배포됩니다. 이 프로그램을 고쳐 배포하거나 네트워크로 제공하면 그 소스도 같은 라이선스로 공개해야 합니다. 이 프로그램에는 어떠한 보증도 없습니다.

Magpie Slicer Next는 다음 프로젝트의 작업 위에 있습니다.

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer): SoftFever와 OrcaSlicer 기여자들 (AGPL-3.0). 이 저장소의 대부분의 코드입니다.
- [Bambu Studio](https://github.com/bambulab/BambuStudio): Bambu Lab (AGPL-3.0). OrcaSlicer의 바탕입니다.
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer): Prusa Research (AGPL-3.0). Bambu Studio의 바탕이며, 레진 서포트 모드에 PrusaSlicer 2.9.6의 SLA 지지대 코드가 들어 있습니다.
- [Slic3r](https://github.com/slic3r/Slic3r): Alessandro Ranellucci와 기여자들 (AGPL-3.0).
- [CuraEngine](https://github.com/Ultimaker/CuraEngine): UltiMaker (AGPL-3.0). Cura 서포트 모드가 CuraEngine의 서포트 코드를 옮겨 왔습니다.

각 원본 파일의 저작권 표기는 그대로 유지됩니다. 사용한 라이브러리의 라이선스는 앱의 정보 > 저작권 창에 있습니다. "OrcaSlicer", "Bambu Studio", "PrusaSlicer", "Cura"는 각 소유자의 이름이며, Magpie Slicer Next는 이들 프로젝트가 만들거나 보증하는 제품이 아닙니다.

모드 SDK의 헤더와 템플릿은 [magpie-mods](https://github.com/Gen7920335/magpie-mods)에서 MIT 라이선스로도 제공되므로, SDK로 만든 모드는 원하는 라이선스로 배포할 수 있습니다.
