# Changelog

All notable changes to Aster are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions follow
[Semantic Versioning](https://semver.org/).

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
