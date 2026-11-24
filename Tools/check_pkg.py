#!/usr/bin/env python3
"""Aster 패키지 관리자 검증.

로컬 디렉터리 레지스트리와 HTTP 레지스트리 왕복을 모두 확인한다.
- publish -> index/아카이브 생성
- install -> sha256 검증, vendor 추출, 전이 의존성 해석, lock 기록
- 버전 불변성(같은 버전 덮어쓰기 거부), 손상된 아티팩트 거부

사용법:
    python3 Tools/check_pkg.py
    python3 Tools/check_pkg.py -v
"""

from __future__ import annotations

import argparse
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_binary() -> Path:
    names = ("aster.exe", "aster") if os.name == "nt" else ("aster", "aster.exe")
    for name in names:
        candidate = ROOT / name
        if candidate.is_file():
            return candidate
    sys.exit("Aster binary not found. Run `make` first.")


BINARY = find_binary()
VERBOSE = False


def log(text: str) -> None:
    if VERBOSE:
        print(text)


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess:
    result = subprocess.run(
        [str(BINARY), *args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    log(f"$ aster {' '.join(args)} (cwd={cwd}) -> {result.returncode}")
    if VERBOSE and result.stdout:
        log(result.stdout.rstrip())
    if VERBOSE and result.stderr:
        log(result.stderr.rstrip())
    return result


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_lib(root: Path, name: str, version: str, body: str,
             deps: list[tuple[str, str]] | None = None,
             registry: str | None = None) -> Path:
    pkg = root / name
    lines = ["# Aster package manifest", f"name = {name}",
             f"version = {version}"]
    if registry:
        lines.append(f"registry = {registry}")
    lines.append("")
    lines.append("[deps]")
    for alias, spec in deps or []:
        lines.append(f"{alias} = {spec}")
    write(pkg / "aster.pkg", "\n".join(lines) + "\n")
    write(pkg / "__init__.ast", body)
    return pkg


class Failure(Exception):
    pass


def expect(cond: bool, message: str) -> None:
    if not cond:
        raise Failure(message)


def expect_output(result: subprocess.CompletedProcess, text: str) -> None:
    if text not in result.stdout:
        raise Failure(
            f"expected {text!r} in stdout, got {result.stdout!r} "
            f"(stderr={result.stderr!r})")


# ---- HTTP 레지스트리 ----


class RegistryHandler(http.server.BaseHTTPRequestHandler):
    directory: Path

    def _target(self) -> Path | None:
        rel = self.path.split("?", 1)[0].lstrip("/")
        if not rel:
            rel = "index.txt"
        parts = rel.split("/")
        if any(p in ("", ".", "..") for p in parts):
            return None
        return self.directory.joinpath(*parts)

    def do_GET(self) -> None:  # noqa: N802
        target = self._target()
        if target is None or not target.is_file():
            self.send_error(404)
            return
        data = target.read_bytes()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self) -> None:  # noqa: N802
        target = self._target()
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        if target is None:
            self.send_error(400)
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        self.send_response(201)
        self.end_headers()

    def log_message(self, *args) -> None:  # noqa: D401
        pass


def start_http_registry(directory: Path):
    handler = type("Handler", (RegistryHandler,), {"directory": directory})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


# ---- 검사 ----


def check_local_registry(root: Path) -> None:
    reg = root / "reg"
    reg.mkdir()
    util = make_lib(root, "util", "0.1.0",
                    'import text;\n\n'
                    'export fn hello() -> str {\n'
                    '    return text.shout("hi");\n'
                    '}\n')
    write(util / "text.ast",
          'export fn shout(str s) -> str {\n    return s + "!";\n}\n')
    result = run(["pkg", "publish", "--registry", str(reg)], util)
    expect(result.returncode == 0, f"publish failed: {result.stderr}")
    expect((reg / "util-0.1.0.astpkg").is_file(), "artifact missing")
    index = (reg / "index.txt").read_text(encoding="utf-8")
    expect("util\t0.1.0\t" in index, f"index entry missing: {index!r}")

    app = root / "app"
    app.mkdir()
    write(app / "aster.pkg",
          "# Aster package manifest\nname = app\nversion = 0.1.0\n"
          f"registry = {reg}\n\n[deps]\n")
    write(app / "main.ast", "import util;\n\nprintln(util.hello());\n")
    result = run(["pkg", "install", "util@0.1.0"], app)
    expect(result.returncode == 0, f"install failed: {result.stderr}")
    expect((app / "vendor/util/__init__.ast").is_file(), "vendor entry missing")
    expect((app / "aster.lock").is_file(), "lock file missing")
    manifest = (app / "aster.pkg").read_text(encoding="utf-8")
    expect("util = registry:0.1.0" in manifest, f"manifest not updated: {manifest}")
    result = run(["main.ast"], app)
    expect(result.returncode == 0, f"run failed: {result.stderr}")
    expect_output(result, "hi!")
    result = run(["pkg", "check"], app)
    expect(result.returncode == 0, f"check failed: {result.stdout}{result.stderr}")
    result = run(["pkg", "install"], app)
    expect(result.returncode == 0, f"second install failed: {result.stderr}")
    expect("cached" in result.stdout, f"install not cached: {result.stdout}")

    geo = make_lib(root, "geo", "0.2.0",
                   'import util;\n\n'
                   'export fn label() -> str {\n'
                   '    return util.hello() + " geo";\n'
                   '}\n',
                   deps=[("util", "registry:0.1.0")])
    result = run(["pkg", "publish", "--registry", str(reg)], geo)
    expect(result.returncode == 0, f"geo publish failed: {result.stderr}")
    index = (reg / "index.txt").read_text(encoding="utf-8")
    expect("util@0.1.0" in index, f"deps column missing: {index!r}")

    app2 = root / "app2"
    app2.mkdir()
    write(app2 / "aster.pkg",
          "# Aster package manifest\nname = app2\nversion = 0.1.0\n"
          f"registry = {reg}\n\n[deps]\n")
    write(app2 / "main.ast", "import geo;\n\nprintln(geo.label());\n")
    result = run(["pkg", "install", "geo@0.2.0"], app2)
    expect(result.returncode == 0, f"geo install failed: {result.stderr}")
    expect((app2 / "vendor/util/__init__.ast").is_file(),
           "transitive dependency not installed")
    manifest = (app2 / "aster.pkg").read_text(encoding="utf-8")
    expect("geo = registry:0.2.0" in manifest and "util = registry:0.1.0" in manifest,
           f"transitive deps missing: {manifest}")
    result = run(["main.ast"], app2)
    expect(result.returncode == 0, f"geo run failed: {result.stderr}")
    expect_output(result, "hi! geo")

    # 같은 버전을 다른 내용으로 덮어쓰기 거부.
    with (geo / "__init__.ast").open("a", encoding="utf-8") as f:
        f.write("\n// changed\n")
    result = run(["pkg", "publish", "--registry", str(reg)], geo)
    expect(result.returncode == 1 and "refusing to overwrite" in result.stderr,
           f"immutability not enforced: {result.returncode} {result.stderr}")
    result = run(["pkg", "publish", "--registry", str(reg), "--force"], geo)
    expect(result.returncode == 0, f"--force publish failed: {result.stderr}")

    # 손상된 아티팩트는 설치를 거부해야 한다.
    artifact = reg / "util-0.1.0.astpkg"
    backup = artifact.read_bytes()
    artifact.write_bytes(backup + b"tampered")
    app3 = root / "app3"
    app3.mkdir()
    write(app3 / "aster.pkg",
          "# Aster package manifest\nname = app3\nversion = 0.1.0\n"
          f"registry = {reg}\n\n[deps]\n")
    result = run(["pkg", "install", "util@0.1.0"], app3)
    expect(result.returncode == 1 and "checksum mismatch" in result.stderr,
           f"tampered artifact accepted: {result.returncode} {result.stderr}")
    expect(not (app3 / "vendor/util/__init__.ast").exists(),
           "tampered artifact was extracted")
    artifact.write_bytes(backup)
    result = run(["pkg", "install", "util@0.1.0"], app3)
    expect(result.returncode == 0, f"restored install failed: {result.stderr}")


def check_bundle(root: Path) -> None:
    pkg = make_lib(root, "bundle", "0.3.0",
                   'export fn value() -> int {\n    return 3;\n}\n')
    result = run(["pkg", "publish"], pkg)
    expect(result.returncode == 0, f"bundle publish failed: {result.stderr}")
    expect((pkg / "dist/bundle-0.3.0.astpkg").is_file(),
           "dist artifact missing")
    expect((pkg / "dist/index.txt").is_file(), "dist index missing")


def check_http_registry(root: Path) -> None:
    if not shutil.which("curl"):
        print("pkg http: skipped (curl not found)")
        return
    remote = root / "remote"
    remote.mkdir()
    server, url = start_http_registry(remote)
    try:
        util = make_lib(root, "httputil", "0.1.0",
                        'export fn hello() -> str {\n    return "http";\n}\n')
        result = run(["pkg", "publish", "--registry", url, "--new"], util)
        expect(result.returncode == 0, f"http publish failed: {result.stderr}")
        expect((remote / "httputil-0.1.0.astpkg").is_file(),
               "http artifact missing")
        expect((remote / "index.txt").is_file(), "http index missing")

        app = root / "httpapp"
        app.mkdir()
        write(app / "aster.pkg",
              "# Aster package manifest\nname = httpapp\nversion = 0.1.0\n"
              f"registry = {url}\n\n[deps]\n")
        write(app / "main.ast", "import httputil;\n\nprintln(httputil.hello());\n")
        result = run(["pkg", "install", "httputil@0.1.0"], app)
        expect(result.returncode == 0, f"http install failed: {result.stderr}")
        result = run(["main.ast"], app)
        expect(result.returncode == 0, f"http run failed: {result.stderr}")
        expect_output(result, "http")

        # 인덱스가 없는 HTTP 레지스트리는 --new 없이는 덮어쓰지 않는다.
        empty = root / "remote2"
        empty.mkdir()
        server2, url2 = start_http_registry(empty)
        try:
            result = run(["pkg", "publish", "--registry", url2], util)
            expect(result.returncode == 1 and "pass --new" in result.stderr,
                   f"missing index guard failed: {result.returncode} {result.stderr}")
        finally:
            server2.shutdown()
            server2.server_close()
    finally:
        server.shutdown()
        server.server_close()


def main() -> int:
    global VERBOSE
    parser = argparse.ArgumentParser()
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    VERBOSE = args.verbose

    checks = [("local", check_local_registry),
              ("bundle", check_bundle),
              ("http", check_http_registry)]
    failures = []
    for name, fn in checks:
        with tempfile.TemporaryDirectory(prefix="aster-pkg-") as tmp:
            root = Path(tmp)
            try:
                fn(root)
                print(f"  ok pkg/{name}")
            except Failure as exc:
                failures.append((name, str(exc)))
                print(f"  FAIL pkg/{name}: {exc}")
    if failures:
        return 1
    print(f"pkg: {len(checks)}/{len(checks)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
