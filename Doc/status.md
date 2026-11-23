# Aster 프로젝트 상태 (Beta 1.0.0)

이 문서는 Beta 1.0.0 시점의 **범위, 호환성 정책, 지원 플랫폼, 알려진 제한**을
정리합니다. 언어 문법은 [language.md](language.md), 표준 라이브러리는
[stdlib.md](stdlib.md), 설치는 [installation.md](installation.md)를 봅니다.

## 1. Beta 1.0.0의 의미

Beta 1.0.0은 **언어와 표준 라이브러리의 기능 동결(feature freeze)** 을 선언하는
릴리스입니다. 1.x 시리즈에서는 다음을 보장합니다.

- 기존 소스가 그대로 파싱·타입 검사·실행됩니다.
- 표준 라이브러리의 공개(export) 이름과 시그니처가 제거되거나 의미가 바뀌지
  않습니다.
- CLI 옵션, 모듈 탐색 규칙, `aster.pkg` 형식이 호환됩니다.

다음은 1.x에서도 바뀔 수 있습니다(비호환으로 취급하지 않음).

- 오류 메시지의 문구와 위치 표시
- `--format`이 만드는 공백·줄바꿈 같은 서식
- 내부 C API, 생성 파일(`Parser/token.c`, `Python/Aster-ast.c` 등)
- 성능, 메모리 사용량, 진단 추가(새 경고/오류는 보수적으로만 추가)
- 문서의 표현과 예제

비호환이 필요한 변경은 다음 major 릴리스에서만 하고, 최소 한 번의 minor
릴리스에서 deprecation 경고를 먼저 제공합니다.

## 2. 지원 플랫폼

| 플랫폼 | 빌드 환경 | 상태 |
| --- | --- | --- |
| Linux (x86_64) | gcc/clang, make | 지원, CI에서 검사 |
| macOS (Intel/Apple Silicon) | Xcode 명령 줄 도구 | 지원, CI에서 검사 |
| Windows (x86_64) | MSYS2 UCRT64 (MinGW-w64) | 지원, CI에서 검사 |
| WSL (Ubuntu) | gcc, make | 지원 |

MSVC는 지원하지 않습니다. Windows에서는 MSYS2 **UCRT64** 셸을 사용합니다.
CI는 Linux에서 AddressSanitizer/UBSan 빌드도 함께 돌립니다.

## 3. 검증

릴리스 전에 다음을 모두 통과해야 합니다.

```sh
make              # C11 빌드 (-Wall -Wextra)
make check        # 프런트엔드, 실행, 코퍼스, REPL, 도구, 무작위, Java 오라클
make check-java   # JDK가 있는 환경에서 Java 오라클 필수 실행
```

`make check`는 고정 코퍼스와 매번 새 시드로 생성하는 무작위 프로그램(정상/거부
케이스 포함)을 함께 검사합니다. CI 구성은 `.github/workflows/ci.yml`에 있습니다.

## 4. 알려진 제한

Beta 1.0.0에서 의도적으로 남겨 둔 제한입니다. 1.x 중 성능·도구 개선은
가능하지만, 아래 의미 변경은 major 릴리스 전에는 하지 않습니다.

### 실행 모델

- CPython과 비슷한 구조의 **트리 워킹 인터프리터**입니다. 바이트코드 VM은
  없습니다.
- AST와 실행 중 만든 값(리스트, 인스턴스 등)은 참조 카운팅/GC 없이 프로세스
  종료 시 해제됩니다. 장시간 실행되는 프로세스는 메모리가 계속 늘 수 있으므로,
  큰 작업은 프로세스를 나눠 실행하는 편이 안전합니다.
- 호출 깊이 기본 상한은 512입니다(`System.setRecursionLimit`으로 조정,
  1..100000). 상한을 크게 올리면 C 스택 한계에 먼저 닿을 수 있습니다.

### 언어·도구

- `--format`은 주석을 보존하지 않습니다.
- 인터페이스 구현 검사는 타입 검사 단계에서만 합니다. `--no-check`로 실행하면
  검사하지 않습니다.
- `package 이름;` 문장은 파싱되지만 로더 동작에는 영향을 주지 않습니다.
- `extern`은 프로세스에 이미 있는 C 심볼만 호출합니다(임의 라이브러리 로드 없음).
- 패키지 관리자는 로컬 경로 기반이며 버전 해석·잠금 파일·원격 다운로드가
  없습니다([packages.md](packages.md) 참조).
- `aster.*` 표준 라이브러리는 Java SE 11 `java.base` API를 옮긴 포팅입니다.
  옮긴 범위 밖의 API는 없으며, 동작은 Java 오라클 테스트로 비교합니다.

## 5. 릴리스 절차

1. `make`와 `make check`(가능하면 `make check-java`)를 통과시킵니다.
2. `README.md`와 이 문서의 버전, `Programs/aster.c`의 `ASTER_VERSION`을
   맞춥니다.
3. `CHANGELOG.md`에 항목을 추가하고 커밋합니다.
4. `vX.Y.Z` 태그를 푸시합니다.
5. `.github/workflows/release.yml`이 Linux/macOS/Windows 아카이브와
   `SHA256SUMS`를 만들어 GitHub Release에 첨부합니다.

## 6. 보안 문제 신고

보안 문제는 공개 이슈로 올리지 말고 메인테이너에게 비공개로 알려 주세요.
재현 방법과 영향을 함께 적으면 확인이 빠릅니다.
