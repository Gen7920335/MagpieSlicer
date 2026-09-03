# Magpie 작업 현황·문서 검증·빌드 백업

기준일: 2026-08-31 (Asia/Seoul). 이 문서는 현재 코드, 기존 실행 기록과 실제 파일을 대조한 정리다. 이번 작업에서는 제품 코드를 수정하거나 새 빌드·슬라이싱·설치·커밋·푸시를 실행하지 않았다.

## 1. 현재 기준

- 저장소: `C:\Users\svadmin\Documents\Codex\2026-08-24\magpie-release-fc07b038`
- 브랜치: `release/2.5.0.0.7`
- HEAD: `968fca1591e5389fdd186c787554699abb3bc5e4` + 보존된 미커밋 변경
- 로컬 설치 후보: `2.5.0.0.8`, 2026-08-28 생성
- 표시/설치파일 버전: `2.5.0.0.8`; Windows 숫자 버전: `2.5.0.8`; 내부 버전: `2.5.0-modified.0.8`
- 버전 소유자: `version.inc`. 일반 Magpie 브랜딩이며 Vulkan 빌드는 ON, profiler/test branding은 OFF다.
- 로컬 README의 공개 릴리스 기록은 0.7을 유지한다. 이번 작업에서 원격 GitHub 상태를 새로 조회하지 않았고, 로컬 후보를 공개했다고 표시하지 않는다.

## 2. 지금까지의 작업 — 기능별 요약

상세 설정·소유 파일은 [기능별 변경점](MAGPIE_FEATURE_CHANGES_2.5.0.0.8.md), 시간순 이력은 [handoff](../CODEX_HANDOFF.md), 발견·수정 기록은 [감사 기록](../artifacts/code-audit-20260828/findings.md)에 있다.

| 기능 | 누적 변경과 현재 상태 |
| --- | --- |
| U1 시작 G-code | 구형 시작 코드 보정, 노즐별 시작 순서와 물리 툴/재질 매핑 검증 기록 보존 |
| 멀티 노즐 | 핫엔드별 구경·선폭, 작은 디테일 라우팅, Classic/Arachne, 인터락킹, 대구경 오버라이드와 미리보기 |
| 저온 인터페이스·타워 | 본체/인터페이스 재질 분리, 온도 전환, 타워 위치·플레이트 이동/삭제와 경계 처리 |
| 인터페이스 확장 | 삼각 패턴, 선 방향, 밀도/간격, 서브레이어, 아랫면 스무딩과 하부 지지 |
| Cura 일반 서포트 | 영역 연결 거리, 벽 수, 선 회전, raft, 자동 수요/페인팅 채널 분리, 취소 전달과 레이어별 단순화 |
| Mixed(auto) | Prusa/Cura + Organic/Slim/Strong/Hybrid 선택, 섬별 베드 도달 커버율, 0~100% 역치, Selective Merge ON/OFF, 두 색 생성기 페인팅 |
| Resin style(auto) | SLA Default/Branching을 FFF로 통합, 침투 깊이를 제외한 설정 노출, 서포트 OFF에서도 모델 띄우기, bridge length 0 처리와 설정 격리 시험 |
| Tree/Organic | 형상이 수용할 수 있는 벽 수만 생성, 가지를 억지로 굵히지 않음, normal/tree 벽 설정 분리, 선택한 노즐 flow 반영, 취소와 회복 진단 정리 |
| GUI·한국어 | stable enum과 선택 인덱스 변환, Mixed/Resin 설정 행 복구, runtime 한국어 카탈로그 갱신 |
| A2L·장치·LESIC | A2L 4개 노즐 계열과 프로파일 의존성 검사, Snapmaker panel과 LESIC 검증 기록 보존 |
| Vulkan | CPU authoritative fallback 유지. Tree/Mixed slice는 결정성 보호를 위해 전체 CPU 경로 |
| Tsunami | 제품 선택지·소스·설정·전용 도구 제거. 기존 프로젝트 로딩 migration과 역사 기록만 유지 |
| 0.8 패키징 | 시작 시 Orca 새 버전 자동 알림 호출만 제거. 수동 업데이트 확인과 프로파일/플러그인 동기화 유지 |

2026-08-28 기록의 같은 설정 비교값은 Cura 전체 약 142.82초 → 37.12초, Mixed Hybrid 전체 약 151.75초 → 17.22초다. 이번 문서 정리에서는 재측정하지 않았으며 모든 모델에 대한 보장으로 일반화하지 않는다.

## 3. 검증 결과와 범위 — 서로 다른 실행을 합치지 않음

| 기록 | 실제 확인한 결과 | 해석 경계 |
| --- | --- | --- |
| 최종 CTest | 393개 항목, 실패 0, Resin driver 1개 의도적 skip | 버전 변경·시작 알림 제거 전 실행. `build-vulkan/Testing/Temporary/LastTest.log`와 `LastTestsDisabled.log` |
| Resin 격리 스윕 | 34/34, 실패·timeout·메모리 제한 이벤트 없음 | [최종 results.json](../artifacts/code-audit-20260828/resin-setting-sweep-final/20260828-162740/results.json) |
| Core readiness | 12/12 | [최종 summary.json](../artifacts/code-audit-20260828/release-readiness-core-final-rerun/summary.json). Mixed Bunny worst-case 11 assertions 포함 |
| 수정 전 Bunny 행렬 | 28/28, Stanford Bunny, 임계각 60도, 기록상 614 layers/최대 Z 118.45 mm | [수정 전 결과](../artifacts/code-audit-20260828/support-features-bunny60/20260828-100729/results.json). 당시 새 실제 선폭 assertion은 미적용; 이후 Classic Tree 폭 결함 발견 |
| 수정 후 support smoke | 28/28, 기본 `tests/data/overhang.obj` | [최종 smoke 결과](../build/verification/support-features/20260828-163821/results.json). G-code 임계각은 60도 19건/70도 3건/90도 6건. 첫 사례 최대 Z 6.25 mm, total layers 39 |
| 0.8 설치 후보 | Release/NSIS 생성 성공, EXE 버전 일치, 설치파일 무결성 및 EXE/DLL 해시 일치 | 설치 실행·GUI 조작·실제 프린터 출력 검증은 아님 |

중요한 정정: **수정 후 support smoke 28/28을 최종 후보의 전체 Bunny/60도 28종 통과로 보고할 수 없다.** 기존 `verify_magpie_release_readiness.ps1`의 support-features 호출은 모델/임계각 override를 넘기지 않는다. 최종 후보로 `ModelPath=resources/handy_models/Stanford_Bunny.drc`, `SupportAngleOverride=60`을 명시한 강화 검증이 남아 있다. 이번 요청은 문서 검증·백업이므로 실행 코드는 바꾸거나 시험을 새로 돌리지 않았다.

`artifacts/code-audit-20260828/ctest-full.log`와 `LastTestsFailed.log`에는 이전 실패가 남아 있다. 이를 삭제하지 않으며, 더 늦은 2026-08-28 16:26의 `LastTest.log`와 혼동하지 않는다. GUI에서 값 변경·드래그·플레이트 이동을 직접 보는 검증도 새로 완료했다고 주장하지 않는다.

## 4. 설치파일 식별

- 원본: `build-vulkan/installer/20260828-174933/MagpieSlicer_Windows_Installer_V2.5.0.0.8_x64.exe`
- 크기: `142092554` bytes
- 설치파일 SHA-256: `26DC29254C43ACBC0BD59067BD140A449D3FC68F03B9E447E68BBDD7D0F083AF`
- `magpie-slicer.exe` SHA-256: `25EAA79DF8A48E6442348217980038DFAF0FC5180E23C41F60D9DB1D581DC3C5`
- `MagpieSlicer.dll` SHA-256: `44DD797EFBA83FF6885B1DB1809BE69578CE7A8FFA20A6415B6045B9BB4F875B`
- [0.8 로컬 릴리스 노트](../release-notes-2.5.0.0.8.md)

## 5. 백업 위치와 복구 범위

백업 루트: `C:\Users\svadmin\Documents\Codex\MagpieBackups\2.5.0.0.8-20260831-094611`

- `installer/`: 기존 0.8 설치파일과 SHA-256 파일
- `build/Release/`: Release 폴더 전체 일반 파일, EXE/DLL/PDB 및 기존 진단 산출물
- `build/Release/resources/`: 15,192개 리소스를 실제 파일로 복사. 원본 junction에 의존하지 않음
- `build-metadata/`: CMake cache, CPack 설정, 버전 헤더, 버전 소스와 설치파일 빌드 스크립트
- `documents-before/`: 이번 수정 전 프로젝트/작업 폴더 MD
- `source-state/`, `tracked-working.patch`: 미커밋 추적 파일의 현재 내용과 binary-capable diff; 삭제 파일 목록은 summary JSON
- `evidence/`: 기존 감사·CTest·Resin·support 시험의 로그와 결과 요약 (대형 G-code 전체 복제는 아님)
- `documents-after/`: 검증을 마친 정리 문서 사본
- `manifest-before.csv`, `manifest-after.csv`, `backup-summary.json`: 상대경로·크기·파일별 SHA-256과 결과

초기 백업은 **16,394개 파일, 2,599,951,206바이트**이며 각 파일의 원본/복사본 SHA-256이 일치했다. 이 숫자는 `documents-after`와 나중에 작성한 manifest/안내문 자체를 제외한다.

이것은 기존 빌드 보존용 복사본이지 새 포터블 배포본이 아니다. 중간 OBJ/대형 정적 라이브러리, 전체 Git 저장소와 의존성 툴체인은 포함하지 않아 완전한 독립 재빌드 환경은 아니다. 같은 C: 드라이브의 별도 폴더이므로 디스크 고장 대비 외장/원격 백업도 아니다.

복구 시 원하는 설치파일 또는 빌드 파일을 별도 위치로 복사하고 manifest 해시를 확인한다. `magpie-slicer-diag.exe`/`MagpieSlicer-check.dll` 등 기존 진단 파일을 정식 `magpie-slicer.exe`/`MagpieSlicer.dll`과 혼동하지 않는다. 실행 중인 DLL이나 현재 작업 트리를 통째로 덮어쓰지 않는다.

## 6. 문서 재검토에서 교정한 내용

- handoff 첫머리의 수정 대기·오래된 실패 수·설치파일 없음 상태를 현재 기록에 맞춤
- 기능 문서와 Mixed 계획의 설치파일 없음 문구를 로컬 후보 존재/미공개로 수정
- 이전 0.7 기능 문서로 향하는 끊어진 현재 링크를 0.8로 수정
- 작업 폴더 인덱스에 현 저장소를 연결하고 2026-08-24 Tsunami 기록은 역사 자료로 구분
- 최종 28종 시험 범위를 실제 입력과 G-code 임계각으로 교차검증하여 과장 표현 교정
- 새 문서 링크·버전·해시·결과 개수·백업 파일 일치를 확인하고, 미검증 사항을 명시

최종 문서 검증의 기계 판독 결과는 `artifacts/document-build-backup-20260831/verification.json`에 기록한다. 기존 기능·실패 기록은 삭제하거나 되돌리지 않았다.
