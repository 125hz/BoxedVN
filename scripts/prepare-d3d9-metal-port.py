#!/usr/bin/env python3
"""Prepare pinned D3D9 Metal sources and audit the port against the iOS ABI.

This development tool never installs DLLs into a Wine runtime or a container.
The report is an integration checklist, not a declaration of compatibility.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def slots(header):
    header = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
    body = re.search(r"enum airconv_unixcalls\s*\{(.*?)\}", header, re.S)
    if not body:
        raise ValueError("Missing airconv call enumeration")
    result = {}
    index = 0
    for token in body[1].split(","):
        token = token.strip()
        if not token:
            continue
        match = re.fullmatch(r"(unix_\w+)(?:\s*=\s*(\d+))?", token)
        if not match:
            raise ValueError("Unrecognized ABI enumerator: " + token)
        if match[2]:
            index = int(match[2])
        if index in result.values():
            raise ValueError("Duplicate native-call slot")
        result[match[1]] = index
        index += 1
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--ios-source", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--fetch", action="store_true")
    args = parser.parse_args()
    pin = json.loads((ROOT / "scripts/dependencies.d3d9-metal.lock.json").read_text())
    if args.fetch and not args.source.exists():
        subprocess.run(["git", "clone", "--depth", "1", "--branch", pin["reference"],
                        pin["repository"], str(args.source)], check=True)
    revision = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin["commit"]:
        raise SystemExit("D3D9 source revision does not match the port pin")
    if subprocess.check_output(["git", "-C", str(args.source), "diff", "--name-only", "HEAD"], text=True).strip():
        raise SystemExit("Audit the pristine source before applying port patches")
    header = Path("src/winemetal/airconv_thunks.h")
    candidate = slots((args.source / header).read_text())
    current = slots((args.ios_source / header).read_text())
    new_calls = {k: v for k, v in candidate.items() if k not in current}
    changed = {k: {"ios": current[k], "candidate": v} for k, v in candidate.items()
               if k in current and current[k] != v}
    native = (args.source / "src/winemetal/unix/winemetal_unix.c").read_text()
    missing_native = [k for k in new_calls if k.removeprefix("unix_") not in native.lower()]
    report = {
        "source": pin,
        "new_shader_calls": new_calls,
        "changed_shader_slots": changed,
        "missing_candidate_native_implementations": missing_native,
        "d3d9_sources": sorted(str(p.relative_to(args.source)).replace("\\", "/")
                              for p in (args.source / "src/d3d9").glob("*.cpp")),
        "enabled_for_runtime": False,
        "required_before_enabling": [
            "Port the matching Metal and DXSO compiler implementation to iOS",
            "Marshal DXSO parameter chains and guest pointers through BoxedWine",
            "Validate both i386 WoW64 and x86-64 call tables and structure layouts",
            "Build matching i386 and x86-64 PE DLLs with the native library",
            "Pass device cube rendering, reset, resource and shader smoke tests",
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Audited {len(report['d3d9_sources'])} D3D9 source files; {len(new_calls)} new shader calls.")
    print("Runtime disabled pending native and WoW64 ABI integration: " + str(args.report))


if __name__ == "__main__":
    main()
