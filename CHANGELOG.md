# Changelog

All notable changes to Aster are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Pointer values now render with their raw slot address in lowercase hexadecimal
  (`<pointer 0x...>`) instead of `<pointer>` when printed or converted with `str`.
- The bytecode VM now reuses per-iteration block environments instead of building
  a new one each pass, skips them entirely when a loop body declares no locals,
  and runs `obj.method(...)` through a dedicated call instruction that does not
  allocate a bound method value.
- Compiled bytecode for a method body is cached once per defining function, so
  bound method calls no longer recompile on every invocation.
- Calls with only positional arguments and no defaults or variadics bind
  parameters directly, without a temporary argument array.
- `switch` and the list/set/dict comprehensions now compile to bytecode instead
  of falling back to the tree walker. Switch cases keep their pattern-match
  semantics (literal, wildcard/`bind`, `when` guards) and comprehension
  accumulators are built on the VM stack; only variant, list, and dict patterns
  still delegate to the tree walker.
- `try`/`catch` without a `finally` block now runs on the VM: the compiler emits
  a handler record for the block, `catch (T e)` type tests run without leaving
  the VM, and a non-matching exception re-throws with the traceback intact.
  `break`/`continue` inside a `try` drop its handler record so later throws in
  the same loop are not caught by a stale one.
- `obj.method(...)` call sites cache the method resolved for the last receiver
  class, so repeated calls on the same class skip property/field/method scans and
  allocate no bound method value. Methods whose body never mentions `super` also
  skip binding it, which removes two GC allocations per call.

### Changed

- The GC allocator now serves small blocks from size-class free lists backed by
  slabs instead of calling `malloc`/`free` per object, which cuts allocation cost
  in allocation-heavy loops while keeping the mark-sweep collector and its
  stress/diagnostic modes unchanged.

## [Beta 2.0.0]

### Added

- Runtime values (strings, lists, tuples, maps, instances, structs, environments,
  and other heap objects) now live in a non-moving mark-sweep garbage-collected
  heap with conservative C-stack roots, typed interpreter/generator roots, and
  typed AST traversal, instead of growing until process exit. `ASTER_GC_STRESS=1`
  collects on every allocation for validation, and `ASTER_GC_TRACE` /
  `ASTER_GC_DEBUG` print collection diagnostics.
- Function and module bodies now run on a bytecode stack VM by default. The
  compiler emits stack-machine instructions for expressions, assignments,
  `if`/`while`/`for`/`for-in` loops, calls, and collections, and hands
  unsupported constructs (class/struct/enum definitions, imports, `try`,
  `switch`, comprehensions, and similar) to the tree walker, so behavior is
  unchanged. `--tree-walk` or `ASTER_TREE_WALK=1` keeps the AST interpreter
  only; `--debug` also uses the tree walker.
- `aster pkg publish` builds a deterministic `.astpkg` archive, hashes it, and
  registers it in a static registry (a directory, `file://` path, or HTTP host
  that accepts `PUT`). `aster pkg install NAME[@VERSION]` resolves transitive
  `registry:` dependencies, verifies SHA-256, extracts packages to `vendor/`,
  and records `aster.lock`. Version immutability, `--force`, `--new`, and
  `--no-upload` (build `dist/` only) are supported.
- `aster.pkg` accepts top-level `registry` and `token_env` keys, and dependency
  values may be written as `registry:VERSION`. Registry and token also come from
  `--registry`/`--token`, `ASTER_REGISTRY`, and `ASTER_REGISTRY_TOKEN`.
- Packages that use `__init__.ast` can now import their own sibling modules, for
  both local-path and vendored registry dependencies.
- `--format --write` formats a file in place, and `--format --check-format`
  reports files that are not normalized (exit code 1) for CI use.
- Diagnostics from the parser, type checker, and runtime now share one format:
  `file:line:col: kind: message` with a source line and caret. Carets align with
  UTF-8 text and tabs.
- The interactive REPL now has line editing (cursor keys, Home/End,
  Backspace/Delete, Ctrl-K/U/W), persistent history in `~/.aster_repl_history`,
  and Tab completion for keywords, globals, and `:` commands. Piped input keeps
  the old line-by-line behavior.

### Changed

- `--format` now preserves line and block comments. Their exact position and
  surrounding blank lines are still normalized.
- Deep recursion now raises `RecursionError` before the C stack is exhausted,
  instead of crashing the process. Generators run with a larger stack.

### Fixed

- On Windows, pressing Ctrl-C while editing a REPL line cancels the line instead
  of terminating the process and leaving the console without echo or line input.
  The console mode is also restored if the process is terminated by a console
  control event.
- On Windows, `Scanner` reads input in VS Code, Windows Terminal, and other
  ConPTY hosts, where Enter arrives as `\r` without a following `\n`. Console
  input is also reset to line mode first, so a terminal left in raw mode by an
  interrupted REPL no longer swallows typed input.
- Accessing an inherited static method or static field on a subclass (for
  example `Derived.make()`) no longer fails with `AttributeError` when the
  subclass was never instantiated.
- On Windows the REPL reads complete VT input sequences, so mouse and window
  reports no longer leave fragments in the line being edited.

## [Beta 1.0.0] - 2026-11-23

First Beta release. The language and standard library are feature-frozen for the
1.x series; see [Doc/status.md](Doc/status.md) for the compatibility policy.

### Added

- `make install` now installs the standard library to
  `<PREFIX>/lib/aster/Lib` alongside `<PREFIX>/bin/aster`, and `make uninstall`
  removes both.
- The runtime locates the standard library relative to the executable, so an
  installed binary works without `ASTER_HOME`.
- `internal` member access is enforced: members marked `internal` are only
  reachable from the module that declares them.
- GitHub Actions CI on Linux, macOS, and Windows (MSYS2 UCRT64), plus an
  AddressSanitizer/UndefinedBehaviorSanitizer job.
- Tag-driven release workflow that builds archives for all three platforms and
  publishes them with `SHA256SUMS`.
- `CONTRIBUTING.md`, `Doc/status.md`, and this changelog.

### Changed

- Version string is now `Beta 1.0.0`.
- `internal` members were previously accessible from any module; code that
  relied on that now reports a type error. Declarations without an access
  modifier are unaffected.

### Fixed

- Installed binaries could fail with `cannot find module` for standard-library
  imports unless `ASTER_HOME` was set. Standard-library discovery now checks the
  executable directory and the installed `lib/aster/Lib` layout.

### Documentation

- Installation guide describes the install layout and the new lookup order.
- Language reference documents enforced `internal` access and the static
  nature of interface implementation checks.
- Standard library reference documents Unicode case mapping, including the
  `ß` to `SS` expansion and the limits of contextual mappings.

## [Alpha 1.5.1] - Previous Alpha release

Base of the Beta 1.0.0 changes. Alpha releases are superseded and are not
covered by the 1.x compatibility policy.
