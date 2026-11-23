# Contributing to Aster

Thanks for your interest in Aster. This document covers building, testing, and
submitting changes.

## Prerequisites

- A C11 compiler (`cc`, `gcc`, or `clang`) and `make`
- Python 3 for `make check` and `make regen` only
- A JDK for the optional Java oracle tests

Windows is supported through **MSYS2 UCRT64**. See
[Doc/installation.md](Doc/installation.md) for per-platform setup. MSVC is not
supported.

## Build

```sh
make
./aster --version
```

The build uses `-std=c11 -O2 -g -Wall -Wextra`. Treat new warnings as bugs.

## Test

```sh
make check
```

This runs the frontend, runtime, library corpus, REPL, tooling, randomized, and
Java oracle checks. On MSYS2, pass `PYTHON=python` if `python3` is missing:

```sh
make check PYTHON=python
```

Useful variants:

```sh
make check VERBOSE=1       # show each case
make check SEED=1234       # reproducible randomized run
make check-java            # require a JDK and run Java oracle tests
```

Failures are preserved under `build/test-failures/`.

## Generated files

Do not edit generated files by hand:

- `Include/internal/token.h`, `Parser/token.c`
- `Include/internal/ast.h`, `Python/Aster-ast.c`

Their sources are `Grammar/Tokens` and `Parser/Aster.asdl`. Change the source,
then run:

```sh
make regen
```

## Making changes

1. Keep changes focused. Match the surrounding C style and naming.
2. Add or update tests under `Lib/test/` whenever behavior changes.
3. Update the relevant document in `Doc/` when syntax, stdlib API, or the CLI
   changes.
4. Run `make` and `make check` before opening a pull request.
5. Describe the motivation and the user-visible effect in the pull request.

If you add a keyword, builtin, token, or module, follow the checklist in
[Doc/extending.md](Doc/extending.md). It lists every file that must be touched,
including the formatter, linter, type checker, interpreter, and tests.

## Compatibility

Aster is in the 1.x Beta series. Do not introduce incompatible changes to
existing syntax or exported standard-library API. Additions are fine; removals
and semantic changes require a deprecation cycle and a major release. See
[Doc/status.md](Doc/status.md).

## Reporting bugs

Include the Aster version (`aster --version`), platform, a minimal `.ast`
reproduction, and the expected versus actual output. For security issues, do
not open a public issue; contact the maintainers privately.
