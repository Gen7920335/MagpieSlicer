# Magpie Slicer 2.5.0.0.8 로컬 설치 후보

이 문서는 2026-08-28에 생성한 로컬 설치 후보의 기록이다. 아직 커밋, 푸시 또는 GitHub 릴리스하지 않았다.

## 변경 요약

- OrcaSlicer 2.5.0 기반 Magpie 공개 버전을 `2.5.0.0.8`로 올렸다.
- 프로그램 시작 시 upstream Orca 애플리케이션 새 버전 확인을 자동 실행하지 않게 했다.
- 프린터 프로파일·플러그인 동기화는 유지한다.
- 도움말 메뉴의 사용자가 직접 실행하는 업데이트 확인은 유지한다.
- 현재 작업 트리의 Mixed, Cura, Tree, Resin 및 멀티 노즐 안정화 변경을 포함한다.

## Windows 설치파일

- 파일: `MagpieSlicer_Windows_Installer_V2.5.0.0.8_x64.exe`
- 크기: `142092554` bytes
- SHA-256: `26DC29254C43ACBC0BD59067BD140A449D3FC68F03B9E447E68BBDD7D0F083AF`
- 표시 버전: `2.5.0.0.8`
- 내부 SemVer 호환 버전: `2.5.0-modified.0.8`

## 패키지 검증

- Release 실행파일과 GUI DLL 빌드 성공.
- 7-Zip NSIS 검사: 15,250개 항목, `Everything is Ok`.
- 설치파일 내부 `magpie-slicer.exe` SHA-256이 빌드 산출물과 일치.
- 설치파일 내부 `MagpieSlicer.dll` SHA-256이 빌드 산출물과 일치.
- 내부 실행파일의 FileVersion과 ProductVersion이 모두 `2.5.0.0.8`임을 확인.
- 설치 또는 UAC 승격, 실제 프린터 출력은 이 패키징 단계에서 수행하지 않았다.

## 2026-08-31 문서 검증과 백업

- 원본 설치파일과 별도 백업본의 SHA-256이 위 기록과 일치한다.
- 빌드의 EXE/DLL, 디버깅 심볼, 전체 리소스, 빌드 메타데이터와 수정 소스 상태를 함께 보존했다.
- 기존 CTest 393개/실패 0/의도적 skip 1개, Resin 34/34, Core 12/12는 버전 변경과 시작 알림 제거 전의 검증이다. 이를 최종 설치본 실행 검증이라고 표현하지 않는다.
- 최종 support 28/28은 작은 overhang 모델의 60/70/90도 smoke 시험이다. 수정 전 전체 Bunny 60도 28/28과 구분하며, 최종 후보 전체 Bunny 행렬은 아직 미검증이다.
- 상세 증거 경로와 복구 범위: [통합 상태·백업 기록](docs/MAGPIE_STATE_AND_BACKUP_2026-08-31.md).
