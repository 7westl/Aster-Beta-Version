# Aster 프로젝트 상태 (Beta 2.0.0)

이 문서는 Beta 2.0.0 시점의 **범위, 호환성 정책, 지원 플랫폼, 알려진 제한**을
정리합니다. 언어 문법은 [language.md](language.md), 표준 라이브러리는
[stdlib.md](stdlib.md), 설치는 [installation.md](installation.md)를 봅니다.

## 1. Beta 2.0.0의 의미

Beta 2.0.0은 1.0.0에서 선언한 **언어와 표준 라이브러리의 기능 동결(feature
freeze)** 을 유지하면서, 실행 엔진을 추적 GC와 바이트코드 VM으로 교체한
릴리스입니다. 2.x 시리즈에서는 다음을 보장합니다.

- 기존 소스가 그대로 파싱·타입 검사·실행됩니다.
- 표준 라이브러리의 공개(export) 이름과 시그니처가 제거되거나 의미가 바뀌지
  않습니다.
- CLI 옵션, 모듈 탐색 규칙, `aster.pkg` 형식이 호환됩니다.

다음은 2.x에서도 바뀔 수 있습니다(비호환으로 취급하지 않음).

- 오류 메시지의 문구와 위치 표시
- `--format`이 만드는 공백·줄바꿈 같은 서식
- 내부 C API, 생성 파일(`Parser/token.c`, `Python/Aster-ast.c` 등)
- 성능, 메모리 사용량, 진단 추가(새 경고/오류는 보수적으로만 추가)
- 문서의 표현과 예제

비호환이 필요한 변경은 다음 major 릴리스(3.0)에서만 하고, 최소 한 번의 minor
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

Beta 2.0.0에서 의도적으로 남겨 둔 제한입니다. 2.x 중 성능·도구 개선은
가능하지만, 아래 의미 변경은 major 릴리스 전에는 하지 않습니다.

### 실행 모델

- **바이트코드 VM**이 기본 실행 경로입니다. 함수 본문과 모듈 본문을 스택 머신
  명령으로 컴파일해 실행하며, VM이 다루지 않는 문법(클래스 정의, `import`,
  `try`, `switch`, 컴프리헨션 등)은 같은 의미의 트리 워커로 넘깁니다.
  `--tree-walk`(또는 `ASTER_TREE_WALK=1`)로 AST 인터프리터만 쓸 수 있고,
  `--debug`도 트리 워커로 실행합니다.
- 실행 중 만든 값(문자열, 리스트, 인스턴스 등)은 **추적 방식 표시-스윕 GC**가
  관리합니다. C 스택과 실행 상태를 루트로 잡으므로 오래 도는 프로그램도 메모리가
  계속 늘지 않습니다. AST는 파싱 단위 아레나에 있어 프로세스 종료 시 해제됩니다.
  `ASTER_GC_STRESS=1`로 모든 할당마다 수집을 강제해 검증할 수 있고,
  `ASTER_GC_TRACE`/`ASTER_GC_DEBUG`로 진단 출력을 볼 수 있습니다.
- 호출 깊이 기본 상한은 512입니다(`System.setRecursionLimit`으로 조정,
  1..100000). 상한을 크게 올려도 실제 C 스택이 바닥나기 전에 `RecursionError`로
  막습니다. 남은 스택 여유는 플랫폼마다 다릅니다.

### 언어·도구

- `--format`은 주석을 보존하지만 빈 줄과 주석의 정확한 위치는 정규화합니다.
- 인터페이스 구현 검사는 타입 검사 단계에서만 합니다. `--no-check`로 실행하면
  검사하지 않습니다.
- `package 이름;` 문장은 파싱되지만 로더 동작에는 영향을 주지 않습니다.
- `extern`은 프로세스에 이미 있는 C 심볼만 호출합니다(임의 라이브러리 로드 없음).
- 패키지 관리자는 정적 레지스트리(디렉터리·HTTP)를 지원하지만, 버전 범위
  해석(`^`, `~` 등)과 레지스트리 측 권한 검사는 없습니다([packages.md](packages.md)
  참조).
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
