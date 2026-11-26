#!/usr/bin/env python3
"""Aster-Core 부분집합 스캐너.

`aster --ast`가 내놓는 덤프를 읽어 각 함수 본문이 부트스트랩 경계 계약
(Doc/core.md)을 만족하는지 판정하고, 코퍼스의 Core 비율을 집계한다.

판정은 두 단계다.

- core: 서명과 본문이 Core 문법만 쓴다. 다른 함수 호출은 호출 지점
  ABI(call-out)로 빠질 수 있으므로 본문 판정을 막지 않는다.
- closed: core이면서 직접 호출(Name 호출)이 모두 같은 파일의 core 함수,
  core extern, Core 빌트인으로만 해석된다. 바이트코드/네이티브 경계 없이
  통째로 컴파일할 수 있는 부분집합이다.

사용법:
    python3 Tools/core_suite.py [--json] [--verdicts] [-v] PATH...
    python3 Tools/core_suite.py Lib/aster Lib/test/test_run
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CORE_PRIMS = {"IntT", "FloatT", "BoolT", "StrT"}
PRIM_NAMES = {
    "IntT": "int",
    "FloatT": "float",
    "BoolT": "bool",
    "StrT": "str",
    "NullT": "null",
}

# Core v0가 직접 ABI로 내보내는 전역 빌트인. 나머지 빌트인 호출은 call-out이다.
CORE_BUILTINS = {"print", "println", "len", "abs", "min", "max", "ord", "chr"}


class DumpError(Exception):
    pass


class Node:
    __slots__ = ("kind", "kw")

    def __init__(self, kind: str, kw: dict):
        self.kind = kind
        self.kw = kw

    def __repr__(self) -> str:  # pragma: no cover - 디버깅용
        args = ", ".join(f"{k}={v!r}" for k, v in self.kw.items())
        return f"{self.kind}({args})"


class DumpParser:
    """`--ast` 덤프 문법의 재귀 하강 파서."""

    _IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    _NUMBER = re.compile(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")

    def __init__(self, text: str):
        self.text = text
        self.i = 0

    def parse(self):
        value = self.value()
        self.ws()
        if self.i != len(self.text):
            raise DumpError(f"trailing input at offset {self.i}")
        return value

    def ws(self) -> None:
        text = self.text
        while self.i < len(text) and text[self.i] in " \t\r\n":
            self.i += 1

    def value(self):
        self.ws()
        if self.i >= len(self.text):
            raise DumpError("unexpected end of dump")
        ch = self.text[self.i]
        if ch == "'":
            return self.string()
        if ch == "[":
            return self.list()
        if ch in "+-" or ch.isdigit():
            if ch in "+-" and not self._looks_numeric(self.i + 1):
                raise DumpError(f"unexpected {ch!r} at offset {self.i}")
            return self.number()
        if ch.isalpha() or ch == "_":
            return self.ident()
        raise DumpError(f"unexpected {ch!r} at offset {self.i}")

    def _looks_numeric(self, pos: int) -> bool:
        if pos >= len(self.text):
            return False
        ch = self.text[pos]
        return ch.isdigit() or ch == "." or ch.isalpha()

    def string(self) -> str:
        text = self.text
        self.i += 1
        start = self.i
        while self.i < len(text):
            ch = text[self.i]
            if ch == "\\":
                self.i += 2
                continue
            if ch == "'":
                value = text[start:self.i]
                self.i += 1
                return value
            self.i += 1
        raise DumpError("unterminated string literal")

    def list(self) -> list:
        self.i += 1
        items = []
        self.ws()
        if self.text[self.i] == "]":
            self.i += 1
            return items
        while True:
            items.append(self.value())
            self.ws()
            ch = self.text[self.i]
            if ch == ",":
                self.i += 1
                continue
            if ch == "]":
                self.i += 1
                return items
            raise DumpError(f"unexpected {ch!r} in list at offset {self.i}")

    def number(self):
        match = self._NUMBER.match(self.text, self.i)
        if not match:
            raise DumpError(f"malformed number at offset {self.i}")
        token = match.group(0)
        self.i = match.end()
        if "." in token or "e" in token or "E" in token:
            return float(token)
        return int(token)

    def ident(self):
        match = self._IDENT.match(self.text, self.i)
        if not match:
            raise DumpError(f"malformed identifier at offset {self.i}")
        name = match.group(0)
        self.i = match.end()
        self.ws()
        if name == "None":
            return None
        if name == "True":
            return True
        if name == "False":
            return False
        if name in ("inf", "Infinity"):
            return float("inf")
        if name in ("nan", "NaN"):
            return float("nan")
        if self.i < len(self.text) and self.text[self.i] == "(":
            return self.call(name)
        raise DumpError(f"bare identifier {name!r} at offset {self.i}")

    def call(self, kind: str) -> Node:
        self.i += 1  # '('
        kw = {}
        self.ws()
        if self.text[self.i] == ")":
            self.i += 1
            return Node(kind, kw)
        while True:
            self.ws()
            match = self._IDENT.match(self.text, self.i)
            if not match:
                raise DumpError(f"expected field name at offset {self.i}")
            key = match.group(0)
            self.i = match.end()
            self.ws()
            if self.text[self.i] != "=":
                raise DumpError(f"expected '=' after {key} at offset {self.i}")
            self.i += 1
            kw[key] = self.value()
            self.ws()
            ch = self.text[self.i]
            if ch == ",":
                self.i += 1
                continue
            if ch == ")":
                self.i += 1
                return Node(kind, kw)
            raise DumpError(f"unexpected {ch!r} in {kind} at offset {self.i}")


def type_desc(ty) -> str:
    if ty is None:
        return "None"
    if not isinstance(ty, Node):
        return repr(ty)
    kind = ty.kind
    kw = ty.kw
    if kind == "Primitive":
        return PRIM_NAMES.get(kw["kind"].kind, kw["kind"].kind)
    if kind == "Named":
        args = kw.get("args") or []
        if args:
            return f"{kw['name']}<{', '.join(type_desc(a) for a in args)}>"
        return kw["name"]
    if kind == "StructRef":
        return kw["tag"]
    if kind == "Pointer":
        return type_desc(kw["base"]) + "*"
    if kind == "Nullable":
        return type_desc(kw["base"]) + "?"
    if kind == "UnionTy":
        return " | ".join(type_desc(m) for m in kw["members"])
    if kind == "FunctionTy":
        params = ", ".join(type_desc(p) for p in kw["params"])
        ret = ret_desc(kw["returns"])
        return f"fn({params}) -> {ret}"
    return kind


def ret_desc(ret) -> str:
    if ret is None:
        return "None"
    if ret.kind == "VoidRet":
        return "void"
    if ret.kind == "ValueRet":
        return type_desc(ret.kw["ty"])
    return ret.kind


def core_type(ty) -> bool:
    return (
        isinstance(ty, Node)
        and ty.kind == "Primitive"
        and ty.kw["kind"].kind in CORE_PRIMS
    )


def is_inferred_type(ty) -> bool:
    return isinstance(ty, Node) and ty.kind == "Named" and ty.kw["name"] in ("var", "let")


class BodyScan:
    """함수 본문의 Core 위반 사유와 직접 호출 이름을 모은다."""

    def __init__(self):
        self.reasons: list[str] = []
        self.calls: list[str] = []
        self.has_callout = False
        self._seen: set[str] = set()

    def add(self, reason: str) -> None:
        if reason not in self._seen:
            self._seen.add(reason)
            self.reasons.append(reason)

    # -- statements --------------------------------------------------------

    def stmts(self, body) -> None:
        for stmt in body or []:
            self.stmt(stmt)

    def stmt(self, n) -> None:
        if n is None:
            return
        kind = n.kind
        kw = n.kw
        if kind == "VarDecl":
            self.var_decl(n)
        elif kind == "Assign":
            self.assign(n)
        elif kind == "ExprStmt":
            self.expr(kw["value"])
        elif kind == "If":
            self.expr(kw["test"])
            self.stmts(kw["body"])
            self.stmts(kw["orelse"])
        elif kind == "While":
            self.expr(kw["test"])
            self.stmts(kw["body"])
        elif kind == "For":
            self.stmt(kw["init"])
            if kw["test"] is not None:
                self.expr(kw["test"])
            self.stmt(kw["step"])
            self.stmts(kw["body"])
        elif kind == "ForRange":
            self.for_range(n)
        elif kind == "ForIn":
            self.add("for-in over a collection")
        elif kind == "Return":
            if kw["value"] is not None:
                self.expr(kw["value"])
        elif kind in ("Break", "Continue"):
            return
        elif kind == "Switch":
            self.add("switch statement")
        elif kind == "Try":
            self.add("try/catch statement")
        elif kind == "Throw":
            self.add("throw statement")
        elif kind == "Yield":
            self.add("generator (yield)")
        elif kind == "Assert":
            self.add("assert statement")
        elif kind == "Destructure":
            self.add("destructuring declaration")
        elif kind == "Import" or kind == "ImportFrom":
            self.add("import inside a function")
        elif kind == "Export":
            self.stmt(kw["body"])
        elif kind == "FunctionDef":
            self.add("nested function definition")
        elif kind in ("ClassDef", "StructDef", "EnumDef", "InterfaceDef", "ExceptionDef"):
            self.add("nested type definition")
        else:
            self.add(f"unsupported statement {kind}")

    def var_decl(self, n) -> None:
        kw = n.kw
        ty = kw["ty"]
        name = kw["name"]
        if ty is None:
            self.add(f"untyped variable '{name}'")
        elif is_inferred_type(ty):
            self.add(f"inferred variable '{name}' (var/let)")
        elif not core_type(ty):
            self.add(f"variable '{name}' has non-Core type {type_desc(ty)}")
        init = kw["init"]
        if init is None:
            return
        if init.kind == "ExprInit":
            self.expr(init.kw["value"])
        elif init.kind == "NoInit":
            return
        else:
            self.add("structured initializer")

    def assign(self, n) -> None:
        kw = n.kw
        target = kw["target"]
        if target.kind == "Name":
            pass
        elif target.kind in ("Attribute", "PtrAttribute", "OptionalAttribute"):
            self.add("attribute assignment")
        elif target.kind == "Subscript":
            self.add("index assignment")
        else:
            self.add("unsupported assignment target")
        self.expr(kw["value"])

    def for_range(self, n) -> None:
        kw = n.kw
        ty = kw["ty"]
        if not core_type(ty):
            self.add(f"loop variable type {type_desc(ty)}")
        it = kw["iter"]
        for part in ("start", "stop", "step"):
            if it is not None and it.kw[part] is not None:
                self.expr(it.kw[part])
        self.stmts(kw["body"])

    # -- expressions -------------------------------------------------------

    def expr(self, n) -> None:
        if n is None:
            return
        kind = n.kind
        kw = n.kw

        if kind == "Constant":
            if kw["value"] is None:
                self.add("null literal")
            return
        if kind in ("Name",):
            return
        if kind == "Self":
            self.add("this reference")
            return
        if kind == "BinOp":
            self.expr(kw["left"])
            self.expr(kw["right"])
            return
        if kind == "UnaryOp":
            self.expr(kw["operand"])
            return
        if kind == "BoolOp":
            self.expr(kw["left"])
            self.expr(kw["right"])
            return
        if kind == "Compare":
            self.expr(kw["left"])
            self.expr(kw["right"])
            return
        if kind == "Ternary":
            self.expr(kw["test"])
            self.expr(kw["body"])
            self.expr(kw["orelse"])
            return
        if kind == "Cast":
            ty = kw["ty"]
            if core_type(ty):
                self.expr(kw["value"])
            else:
                self.add(f"cast to non-Core type {type_desc(ty)}")
            return
        if kind == "Incr":
            target = kw["target"]
            if target.kind != "Name":
                self.add("unsupported increment target")
            return
        if kind == "Call":
            self.call(n)
            return

        # 아래는 Core v0에 없는 식이다. 이유만 남기고 자식은 훑지 않는다.
        disabled = {
            "Lambda": "lambda/closure",
            "Pipe": "pipe expression",
            "Coalesce": "null-coalescing operator",
            "Is": "type test (is)",
            "In": "membership test (in)",
            "AddressOf": "pointer operation",
            "Deref": "pointer operation",
            "SizeOf": "pointer operation",
            "New": "object construction (new)",
            "Attribute": "field access",
            "PtrAttribute": "pointer field access",
            "OptionalAttribute": "optional field access",
            "Array": "array literal",
            "List": "list literal",
            "Tuple": "tuple literal",
            "Dict": "dict literal",
            "Set": "set literal",
            "Spread": "spread",
            "ListComp": "list comprehension",
            "SetComp": "set comprehension",
            "DictComp": "dict comprehension",
            "Range": "range expression",
            "Subscript": "index access",
            "Slice": "slice",
            "FString": "formatted string",
            "FormattedValue": "formatted string",
        }
        reason = disabled.get(kind)
        if reason is not None:
            self.add(reason)
        else:
            self.add(f"unsupported expression {kind}")

    def call(self, n) -> None:
        kw = n.kw
        func = kw["func"]
        if kw["keywords"]:
            self.add("call with named argument")
        for arg in kw["args"]:
            self.expr(arg)
        for item in kw["keywords"]:
            self.expr(item.kw["value"])
        if func.kind == "Name":
            self.calls.append(func.kw["id"])
        elif func.kind in ("Attribute", "PtrAttribute", "OptionalAttribute"):
            self.has_callout = True
            self.expr(func.kw["value"])
        elif func.kind == "Self":
            self.add("call through this")
        else:
            self.add("indirect call")


def signature_reasons(fn) -> list[str]:
    reasons = []
    if fn.kw["type_params"]:
        reasons.append("generic type parameters")
    for param in fn.kw["params"]:
        ty = param.kw["ty"]
        name = param.kw["name"]
        if param.kw["variadic"]:
            reasons.append(f"variadic parameter '{name}'")
        if param.kw["default_value"] is not None:
            reasons.append(f"default value for parameter '{name}'")
        if ty is None:
            reasons.append(f"untyped parameter '{name}'")
        elif not core_type(ty):
            reasons.append(f"parameter '{name}' has non-Core type {type_desc(ty)}")
    ret = fn.kw["returns"]
    if ret is None:
        reasons.append("missing return type")
    elif ret.kind == "VoidRet":
        pass
    elif ret.kind == "ValueRet":
        if not core_type(ret.kw["ty"]):
            reasons.append(f"non-Core return type {type_desc(ret.kw['ty'])}")
    else:
        reasons.append(f"unsupported return form {ret.kind}")
    return reasons


class Verdict:
    __slots__ = ("name", "kind", "reasons", "calls", "has_callout", "closed")

    def __init__(self, name: str, kind: str, reasons, calls, has_callout: bool):
        self.name = name
        self.kind = kind
        self.reasons = list(reasons)
        self.calls = list(calls)
        self.has_callout = has_callout
        self.closed = False

    @property
    def core(self) -> bool:
        return not self.reasons

    def to_json(self) -> dict:
        return {
            "name": self.name,
            "kind": self.kind,
            "core": self.core,
            "closed": self.closed,
            "reasons": self.reasons,
            "calls": self.calls,
            "callout": self.has_callout,
        }


def classify_function(name: str, kind: str, fn) -> Verdict:
    scan = BodyScan()
    scan.stmts(fn.kw["body"])
    reasons = signature_reasons(fn) + scan.reasons
    deduped = []
    seen = set()
    for reason in reasons:
        if reason not in seen:
            seen.add(reason)
            deduped.append(reason)
    return Verdict(name, kind, deduped, scan.calls, scan.has_callout)


def excluded_verdict(name: str, kind: str, reason: str) -> Verdict:
    return Verdict(name, kind, [reason], [], False)


def unwrap_export(stmt):
    if stmt is not None and stmt.kind == "Export":
        return stmt.kw["body"]
    return stmt


def collect(module):
    """모듈에서 함수 판정과 extern 목록을 뽑는다."""
    verdicts: list[Verdict] = []
    externs: list[tuple[str, bool]] = []
    for stmt in module.kw["body"]:
        stmt = unwrap_export(stmt)
        if stmt is None:
            continue
        if stmt.kind == "FunctionDef":
            if stmt.kw["is_extern"]:
                externs.append((stmt.kw["name"], not signature_reasons(stmt)))
            else:
                verdicts.append(classify_function(stmt.kw["name"], "function", stmt))
        elif stmt.kind == "ClassDef":
            verdicts.extend(class_methods(stmt))
        elif stmt.kind == "StructDef":
            for member in stmt.kw["members"]:
                if member.kind == "StructCtor":
                    verdicts.append(
                        excluded_verdict(
                            f"{stmt.kw['tag']}.new",
                            "struct constructor",
                            "struct constructor (object ABI is outside Core v0)",
                        )
                    )
        elif stmt.kind == "EnumDef":
            for member in stmt.kw["methods"]:
                if member.kind == "Method":
                    verdicts.append(
                        excluded_verdict(
                            f"{stmt.kw['name']}.{member.kw['name']}",
                            "enum method",
                            "enum method (object ABI is outside Core v0)",
                        )
                    )
    return verdicts, externs


def class_methods(cls) -> list[Verdict]:
    verdicts = []
    cname = cls.kw["name"]
    for member in cls.kw["members"]:
        if member.kind == "Method":
            name = f"{cname}.{member.kw['name']}"
            if member.kw["is_static"]:
                verdicts.append(classify_function(name, "static method", member))
            else:
                verdicts.append(
                    excluded_verdict(
                        name,
                        "method",
                        "instance method (object ABI is outside Core v0)",
                    )
                )
        elif member.kind == "Constructor":
            verdicts.append(
                excluded_verdict(
                    f"{cname}.new",
                    "constructor",
                    "constructor (object ABI is outside Core v0)",
                )
            )
        elif member.kind in ("Property", "Getter", "Setter"):
            verdicts.append(
                excluded_verdict(
                    f"{cname}.{member.kw['name']}",
                    "accessor",
                    "property accessor (object ABI is outside Core v0)",
                )
            )
    return verdicts


def mark_closed(verdicts: list[Verdict], externs) -> None:
    """직접 호출이 모두 같은 파일 Core로 해석되는 함수를 최대 고정점으로 고른다.

    오버로드는 이름이 같으므로, 호출은 후보 중 같은 이름이 전부 Core일 때만
    해석된 것으로 본다.
    """
    defs = collections.defaultdict(list)
    for verdict in verdicts:
        if verdict.kind in ("function", "static method"):
            defs[verdict.name].append(verdict)
    externs_by_name = collections.defaultdict(list)
    for name, is_core in externs:
        externs_by_name[name].append(is_core)
    extern_core = {name for name, flags in externs_by_name.items() if all(flags)}
    candidates = {v.name for v in verdicts if v.core and not v.has_callout}
    changed = True
    while changed:
        changed = False
        for name in list(candidates):
            for verdict in defs[name]:
                for callee in verdict.calls:
                    if callee in CORE_BUILTINS or callee in extern_core:
                        continue
                    if callee in candidates and all(
                        item.core for item in defs.get(callee, [])
                    ):
                        continue
                    candidates.discard(name)
                    changed = True
                    break
                if name not in candidates:
                    break
    for verdict in verdicts:
        verdict.closed = verdict.name in candidates


def scan_file(binary: Path, path: Path):
    proc = subprocess.run(
        [str(binary), "--ast", str(path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.returncode != 0:
        detail = (proc.stderr or "").strip().splitlines()
        message = detail[0] if detail else "aster --ast failed"
        return None, message
    try:
        tree = DumpParser(proc.stdout).parse()
    except DumpError as exc:
        return None, f"cannot parse AST dump: {exc}"
    if not isinstance(tree, Node) or tree.kind != "Module":
        return None, "AST dump is not a module"
    verdicts, externs = collect(tree)
    mark_closed(verdicts, externs)
    return verdicts, None


def find_binary(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit)
        if not candidate.is_file():
            sys.exit(f"binary not found: {candidate}")
        return candidate
    names = ("aster.exe", "aster") if os.name == "nt" else ("aster", "aster.exe")
    for name in names:
        candidate = ROOT / name
        if candidate.is_file():
            return candidate
    sys.exit("Aster binary not found. Run `make` first.")


def gather_sources(paths) -> list[Path]:
    files = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            files.extend(sorted(path.rglob("*.ast")))
        elif path.is_file():
            files.append(path)
        else:
            sys.exit(f"no such file or directory: {raw}")
    seen = set()
    unique = []
    for path in files:
        key = path.resolve()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def verdict_line(verdict: Verdict) -> str:
    if verdict.core:
        suffix = "closed" if verdict.closed else "call-out"
        return f"{verdict.name}: core ({suffix})"
    return f"{verdict.name}: fallback ({'; '.join(verdict.reasons)})"


def print_report(results, verbose: bool) -> None:
    total = core = closed = files = failed = 0
    reasons = collections.Counter()
    for path, verdicts, error in results:
        shown = path.as_posix()
        if error is not None:
            failed += 1
            print(f"{shown}: error: {error}")
            continue
        files += 1
        fcore = sum(1 for v in verdicts if v.core)
        fclosed = sum(1 for v in verdicts if v.closed)
        total += len(verdicts)
        core += fcore
        closed += fclosed
        for verdict in verdicts:
            for reason in verdict.reasons:
                reasons[reason] += 1
        if verdicts:
            share = fcore * 100 // len(verdicts)
            print(
                f"{shown}: {len(verdicts)} functions, core {fcore} ({share}%), "
                f"closed {fclosed}"
            )
        else:
            print(f"{shown}: no functions")
        if verbose:
            for verdict in verdicts:
                print(f"  {verdict_line(verdict)}")
    if total:
        pct = core * 100 // total
        cpct = closed * 100 // total
        print(
            f"total: {total} functions in {files} files, "
            f"core {core} ({pct}%), closed {closed} ({cpct}%)"
        )
        if reasons:
            print("reasons:")
            for reason, count in reasons.most_common(12):
                print(f"  {count:4d}  {reason}")
    if failed:
        sys.exit(1)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Aster-Core 부분집합 판정 및 코퍼스 측정",
    )
    parser.add_argument("paths", nargs="+", help="Aster 소스 파일 또는 디렉터리")
    parser.add_argument("--json", action="store_true", help="JSON으로 출력")
    parser.add_argument(
        "--verdicts",
        action="store_true",
        help="파일별 함수 판정만 한 줄씩 출력 (테스트용)",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="함수별 판정 출력")
    parser.add_argument("--binary", help="aster 실행 파일 경로")
    args = parser.parse_args(argv)

    binary = find_binary(args.binary)
    results = []
    for path in gather_sources(args.paths):
        verdicts, error = scan_file(binary, path)
        results.append((path, verdicts or [], error))

    if args.json:
        payload = {
            "files": [
                {
                    "path": path.as_posix(),
                    "error": error,
                    "functions": [v.to_json() for v in verdicts],
                }
                for path, verdicts, error in results
            ],
            "totals": {
                "files": sum(1 for _, _, error in results if error is None),
                "functions": sum(len(v) for _, v, error in results if error is None),
                "core": sum(
                    sum(1 for item in v if item.core)
                    for _, v, error in results
                    if error is None
                ),
                "closed": sum(
                    sum(1 for item in v if item.closed)
                    for _, v, error in results
                    if error is None
                ),
            },
        }
        json.dump(payload, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0 if all(error is None for _, _, error in results) else 1

    if args.verdicts:
        ok = True
        for path, verdicts, error in results:
            if error is not None:
                print(f"{path}: error: {error}", file=sys.stderr)
                ok = False
                continue
            for verdict in verdicts:
                print(verdict_line(verdict))
        return 0 if ok else 1

    print_report(results, args.verbose)
    return 0


if __name__ == "__main__":
    sys.exit(main())
