# Aster 패키지 관리자

Aster의 패키지 관리자는 프로젝트 의존성을 `aster.pkg` 파일에 기록하고, 실행할
때 그 경로를 모듈 탐색 경로에 추가합니다. 원격 배포는 **정적 레지스트리**를
사용합니다. 레지스트리는 `index.txt`와 `.astpkg` 아카이브를 두는 디렉터리나
HTTP 호스트일 뿐이고, 별도 서버 프로세스가 필요하지 않습니다.

- 로컬 경로 의존성: 지금처럼 `별칭 = 경로`
- 레지스트리 의존성: `별칭 = registry:버전`
- 배포: `aster pkg publish` → 아카이브 생성 + 인덱스 갱신
- 설치: `aster pkg install` → 내려받기 + SHA-256 검증 + `vendor/` 추출

## 1. 빠른 시작

```sh
mkdir myapp
cd myapp
aster pkg init myapp
```

`aster.pkg`가 만들어집니다.

```ini
# Aster package manifest
name = myapp
version = 0.1.0

[deps]
```

로컬 의존성을 추가합니다.

```sh
aster pkg add util ../shared
```

`../shared`에서 `util.ast` 또는 `util/__init__.ast`를 찾을 수 있어야 합니다.
패키지 디렉터리 자체(`../shared/util`)를 넘겨도 됩니다.

레지스트리에서 설치합니다.

```sh
aster pkg install util@0.1.0 --registry https://example.com/aster-registry
```

```aster
import util;
println(util.hello());
```

## 2. 명령

| 명령 | 설명 |
| --- | --- |
| `aster pkg init [name]` | 현재 디렉터리에 `aster.pkg`를 만듭니다. 이름 기본값은 `app`. |
| `aster pkg add NAME PATH` | 로컬 경로 의존성을 추가합니다. |
| `aster pkg add NAME[@VER]` | 레지스트리 패키지를 설치합니다(`install`과 같음). |
| `aster pkg install [NAME[@VER]]` | 레지스트리 의존성을 `vendor/`에 설치합니다. |
| `aster pkg publish` | 아카이브를 만들어 레지스트리 인덱스를 갱신합니다. |
| `aster pkg remove NAME` | 의존성을 제거합니다. |
| `aster pkg list` | 이름, 버전, 의존성을 출력합니다. |
| `aster pkg check` | 각 의존성이 실제로 import 가능한지 검사합니다. |
| `aster pkg -h` | 도움말. |

`aster pkg`는 소스를 실행하지 않습니다.

### `install` 옵션

```sh
aster pkg install [NAME[@VER]] [--registry URL] [--token TOKEN]
```

- 인자 없이 실행하면 `aster.pkg`의 모든 `registry:` 의존성을 설치합니다.
  버전이 `latest`이면 `aster.lock`에 기록된 버전을 먼저 사용합니다.
- `NAME@VER`를 주면 그 패키지와 전이 의존성을 설치합니다. 버전을 생략하면
  레지스트리의 가장 높은 버전을 고릅니다.
- 해석된 모든 패키지는 `vendor/<이름>/`에 풀리고, 정확한 버전이
  `aster.pkg`(`registry:버전`)와 `aster.lock`에 기록됩니다.
- 이미 같은 버전·SHA-256으로 설치되어 있으면 `cached`로 건너뜁니다.
- 내려받은 아카이브의 SHA-256이 인덱스와 다르면 설치를 중단합니다.

### `publish` 옵션

```sh
aster pkg publish [--registry URL] [--token TOKEN] [--out DIR]
                  [--force] [--new] [--no-upload]
```

- `dist/<이름>-<버전>.astpkg`를 만들고 해시를 계산합니다.
- `--registry`가 없으면 업로드하지 않고 `dist/index.txt`만 갱신합니다. 두
  파일을 직접 정적 호스트에 올리면 됩니다.
- `--no-upload`는 레지스트리가 설정되어 있어도 업로드 없이 `dist/`에만
  씁니다.
- HTTP 레지스트리에 `index.txt`가 없으면 `--new`가 필요합니다.
- 같은 이름·버전이 이미 있고 SHA-256이 다르면 기본적으로 거부하며,
  `--force`로 덮어쓸 수 있습니다.
- 업로드 순서는 아카이브 → 인덱스입니다.

## 3. `aster.pkg` 형식

`key = value` 줄과 `[deps]` 섹션으로 구성됩니다. `#`부터 줄 끝까지는 주석입니다.
값의 양끝에 따옴표를 써도 됩니다.

```ini
# Aster package manifest
name = myapp
version = 1.2.0
registry = https://example.com/aster-registry
token_env = ASTER_TOKEN

[deps]
util = registry:0.1.0
network = libs/network
```

- `name`, `version`은 선택입니다. `publish`에는 둘 다 필요합니다.
- `registry`는 의존성을 받아올 정적 레지스트리 위치입니다(URL 또는 경로).
- `token_env`는 HTTP 업로드에 쓸 토큰을 담은 환경 변수 이름입니다.
- `registry:버전` 값은 레지스트리 의존성입니다. `registry:latest`도 쓸 수
  있지만, `publish`하려면 정확한 버전으로 고정해야 합니다.
- 그 밖의 값은 로컬 경로입니다. 상대 경로는 **`aster.pkg`가 있는 디렉터리**를
  기준으로 해석됩니다.
- `aster pkg add`/`remove`/`install`은 파일을 다시 쓸 때 주석과 알 수 없는 키를
  보존하지 않습니다.

레지스트리와 토큰은 다음 우선순위로 정해집니다.

1. `--registry` / `--token`
2. `ASTER_REGISTRY` / `ASTER_REGISTRY_TOKEN`
3. `aster.pkg`의 `registry` / `token_env`가 가리키는 환경 변수

## 4. 레지스트리 형식

정적 레지스트리는 다음 두 종류의 파일만 있으면 됩니다.

```text
registry/
  index.txt
  util-0.1.0.astpkg
  geo-0.2.0.astpkg
```

`index.txt`는 헤더 주석과 탭으로 구분된 줄로 이루어집니다.

```text
# aster registry index v1
# name	version	sha256	file	deps
geo	0.2.0	<sha256>	geo-0.2.0.astpkg	util@0.1.0
util	0.1.0	<sha256>	util-0.1.0.astpkg
```

- `sha256`: 아카이브 전체의 SHA-256(소문자 16진수).
- `file`: 레지스트리 기준 아카이브 경로. 절대 URL도 가능합니다.
- `deps`: 쉼표로 구분한 `별칭@버전` 목록. 비어 있을 수 있습니다.
- `publish`가 인덱스를 이름·버전 순으로 정렬해 다시 씁니다.

`.astpkg`는 **압축하지 않은 ustar tar**입니다. 파일 순서를 정렬하고
수정 시각·소유자 정보를 0으로 맞추므로, 같은 소스에서 같은 SHA-256이
나옵니다. 아카이브에는 `.ast` 파일과 `aster.pkg`만 들어가며 `vendor/`,
`dist/`, `build/`, `.git/`, `tmp/`, `node_modules/` 등은 제외됩니다.

### HTTP 레지스트리

`http://` 또는 `https://` 레지스트리는 `publish`할 때 외부 `curl`로 업로드
합니다.

- `GET <registry>/index.txt`, `GET <registry>/<파일>`로 읽습니다.
- `PUT <registry>/<파일>`로 아카이브와 인덱스를 올립니다.
- `--token`/`token_env`가 있으면 `Authorization: Bearer <토큰>` 헤더를
  붙입니다. 읽기 요청에는 붙지 않습니다.
- GitHub Releases처럼 `PUT`을 받지 않는 정적 호스팅은 `--no-upload`로
  `dist/`만 만든 뒤 직접 업로드하세요.

로컬 경로나 `file://` 레지스트리는 `curl` 없이 파일 복사로 동작합니다.

## 5. 탐색 규칙

실행 파일은 주 스크립트가 있는 디렉터리에서 위로 올라가며 가장 가까운
`aster.pkg`를 찾습니다(최대 32단계). 그 파일의 각 의존성을 모듈 탐색
경로에 추가합니다.

모듈 탐색 순서는 다음과 같습니다.

1. 주 스크립트의 디렉터리
2. 표준 라이브러리 (`ASTER_HOME/Lib`, 없으면 현재 디렉터리의 `Lib`)
3. `aster.pkg`의 의존성 경로들
   - 로컬 경로: 그 경로, 그리고 `<경로>/<별칭>` 디렉터리
   - 레지스트리: `vendor/`, 그리고 `vendor/<별칭>`
4. `ASTER_PATH` 환경 변수의 경로들

`ASTER_PATH`는 Windows에서는 `;`, Unix에서는 `;`와 `:`로 구분합니다.

```sh
export ASTER_PATH=/opt/aster-libs:$HOME/aster-libs
```

`import util;`은 각 경로에서 `util.ast`를 먼저, 그다음 `util/__init__.ast`를
찾습니다. `import a.b;`는 `a/b.ast` 또는 `a/b/__init__.ast`를 찾습니다.
`__init__.ast` 안에서 같은 패키지의 형제 모듈을 import하려면 위의
`<경로>/<별칭>` 또는 `vendor/<별칭>` 항목이 필요하며, 자동으로 추가됩니다.

`import util;`은 `aster.pkg`의 별칭과 레지스트리 패키지 이름이 일치할 때만
동작합니다.

## 6. 패키지 만들기

패키지는 디렉터리와 `__init__.ast`로 구성할 수 있습니다.

```text
shared/
  aster.pkg
  __init__.ast        # export fn hello() -> str { ... }
  text.ast
```

```aster
# shared/text.ast
export fn shout(str s) -> str { return s.upper() + "!"; }

# shared/__init__.ast
import text;
export fn hello() -> str { return text.shout("hello"); }
```

```ini
# shared/aster.pkg
name = shared
version = 0.1.0

[deps]
```

배포하고 설치합니다.

```sh
cd shared
aster pkg publish --registry ../registry

cd ../app
aster pkg install shared@0.1.0 --registry ../registry
```

전이 의존성이 있는 패키지는 매니페스트에 정확한 버전으로 적고 publish합니다.

```ini
[deps]
util = registry:0.1.0
```

## 7. 한계

- 버전 **범위**(`^`, `~`, `>=`)는 지원하지 않습니다. 정확한 버전 또는
  `latest`만 씁니다.
- 전이 의존성 충돌은 그래프 전체를 풀지 않고 **높은 버전 우선**으로
  근사합니다. 서로 호환되지 않는 요구가 있으면 직접 버전을 맞추세요.
- 아카이브 서명, 게시 취소, 사용자 계정 관리는 없습니다. 접근 제어는
  레지스트리 호스트(프록시, 토큰)에서 처리해야 합니다.
- HTTP 읽기에는 토큰을 보내지 않으므로, 비공개 레지스트리는 `file://`이나
  인증 프록시를 사용하세요.
- 의존성의 의존성은 인덱스의 `deps` 열로만 해석합니다. 인덱스에 없는
  패키지는 설치할 수 없습니다.
- 경로는 실행 시점에 해석되므로 프로젝트를 옮기면 상대 경로를 확인해야
  합니다.
