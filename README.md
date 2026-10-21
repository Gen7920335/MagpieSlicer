<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer.png" width="150">

# Magpie Slicer (Next)

OrcaSlicer 나이틀리 위에 모드(mod)로 기능을 얹은 슬라이서

</div>

> 개발 중인 2세대 라인입니다. 아직 배포 전이며, 실제 장비에 보내기 전에 미리보기와 생성된 G-code를 꼭 확인하십시오.

## 1세대(Beta 7)와 다른 점

- **OrcaSlicer 나이틀리 기반:** Orca 코드를 직접 고치지 않고, Magpie 기능은 모드로 켜고 끕니다. 모드를 모두 끄면 Orca와 같은 방식으로 슬라이스합니다(출력과 실사용에 문제없는 Orca 버그 수정은 항상 적용).
- **따로 설치:** 앱 키 `MagpieSlicerNext`, 실행 파일 `magpie-slicer-next`, 설치 이름 "Magpie Slicer Next". 설정 폴더와 파일 연결을 OrcaSlicer·1세대와 공유하지 않습니다. `orcaslicer://` 링크는 그대로 처리합니다.
- **Orca 사용자 사전 설정 가져오기:** 처음 실행할 때 OrcaSlicer 데이터 폴더의 사용자 사전 설정을 한 번 복사합니다. 이미 있는 파일은 덮어쓰지 않습니다. 파일 > 가져오기 > "OrcaSlicer 사용자 사전 설정 가져오기"로 다시 실행할 수 있습니다.
- **1세대 설정 이전:** 1세대 프로젝트·사전 설정을 열면 Magpie 전용 설정이 해당 모드 설정으로 옮겨집니다.
- **GPU:** 쓰지 않습니다. Vulkan에 이어 CUDA도 뺐습니다(측정 결과 CPU 경로가 더 빠르거나 같았음).

## 기본 모드

| 모드 | 하는 일 |
| --- | --- |
| 다중 노즐 벽 | 노즐 크기가 다른 툴헤드를 함께 써서 날카로운 외벽은 작은 노즐, 나머지는 큰 노즐로 출력 |
| 절반 높이 외벽과 서포트 (H/2) | 바깥 벽 2줄과 서포트를 레이어 높이의 절반으로 두 번 출력 |
| Cura 서포트 | Cura 방식의 연속 경로 일반 서포트 |
| 혼합 서포트 | 베드에 닿는 곳은 일반, 나머지는 트리 서포트로 결합 |
| 레진 서포트 | PrusaSlicer SLA 지지대 방식을 FFF 서포트로 출력 |
| 저온 서포트 접점 | 노즐 하나로 모델과 서포트 접점을 다른 온도로 출력, 온도 하강 타워 포함 |
| 서포트 추가 설정 | 삼각형 접점 패턴, 접점 서브레이어(패턴·각도·온도), 일반·트리 서포트 벽 수 |
| Snapmaker U1 | U1에서 순정 시작 순서(PRINT_START, 홈, 베드 메시, 퍼지)가 빠진 시작 G-code를 보호 |

그 밖에 LESIC(온도·체적 유량 캘리브레이션), Snapmaker U1 모니터 플러그인, 한국어 번역이 들어 있습니다.

## 빌드 (Windows)

```bat
build_win.bat -s --tests
```

- 테스트: `build` 폴더에서 `ctest -C Release -j8`
- Orca와의 출력 비교: `scripts/magpie/golden/golden.py`
- 무작위 조합 점검: `scripts/magpie/sweep/random_sweep.py`
- 설치파일: `scripts/build_installer.ps1`

개발 규칙은 [AGENTS.md](AGENTS.md)와 [docs/PROJECT_CONTRACTS.md](docs/PROJECT_CONTRACTS.md), 진행 상황은 [CODEX_HANDOFF.md](CODEX_HANDOFF.md)에 있습니다. OrcaSlicer 원래 안내문은 [README_ORCASLICER.md](README_ORCASLICER.md)에 있습니다.

## 라이선스

OrcaSlicer와 같은 GNU AGPL v3입니다. OrcaSlicer는 Bambu Studio, PrusaSlicer, Slic3r를 바탕으로 합니다.
