# Aster

**A modern general-purpose programming language.**

Aster is a general-purpose programming language designed to provide a flexible, expressive, and extensible programming environment.

---

## Overview

Aster is a programming language project focused on building an independent language ecosystem.

**Current status: Beta 1.0.0.** The language and standard library are feature-frozen
for the 1.x series: from this release on, existing syntax and exported API are not
changed in incompatible ways within 1.x. See [Project Status](Doc/status.md) for the
release policy and known limits.

## Installation

The recommended way to install Aster is through **Aster Setup Wizard**.

### Windows

1. Download the latest Aster Setup Wizard.
2. Run the installer.
3. Follow the instructions displayed in the installation wizard.
4. Complete the installation.
5. Open a terminal and verify the installation:

```bash
aster --version
```

**Download:** [Aster Setup Wizard](https://www.mediafire.com/file/l6m6hvc8zwjn1zd/Aster_Setup_Wizard.exe/file)

For manual installation, building from source, and `make install` layouts, see the
[Installation Guide](Doc/installation.md).

## Quick Start

Create a file named `hello.ast`:

```aster
print("Hello, World!");
```

Run the program:

```bash
aster hello.ast
```

## Documentation

| Document                                    | Description                              |
| ------------------------------------------- | ---------------------------------------- |
| [Language Reference](Doc/language.md)       | Language syntax and specifications       |
| [Standard Library](Doc/stdlib.md)           | Standard library reference               |
| [Installation Guide](Doc/installation.md)   | Manual installation and build setup      |
| [Package Manager](Doc/packages.md)          | Local dependency manifests (`aster.pkg`) |
| [Extending Aster](Doc/extending.md)         | Adding keywords, builtins, modules       |
| [Project Status](Doc/status.md)             | Beta scope, release policy, limits       |

## Contributing

Build and test instructions are in [CONTRIBUTING.md](CONTRIBUTING.md). All changes
must pass `make check`.

## Version

**Current Version:** `Beta 1.0.0`

Release history is in [CHANGELOG.md](CHANGELOG.md).

## License

This project is licensed under the terms specified in [LICENSE](LICENSE).

---

<p align="center">
  <b>Aster Programming Language</b>
</p>
