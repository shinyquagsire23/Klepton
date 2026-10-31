#!/usr/bin/env python3
"""Fetch the supplied CoreMods dependency closure and inventory real ELF imports.

Downloads only the URLs in QMOD manifests; never installs or executes a mod.
The lock records hashes, manifest constraints and conflicts for review.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

from audit_elf import Elf


def version_tuple(value):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\+[^ ]+)?", value)
    return tuple(map(int, match.groups())) if match else None


def satisfies(version, constraint):
    """Only claim compatibility for the exact/caret ranges these manifests use."""
    if constraint == "*":
        return True
    exact = constraint.lstrip("=^")
    actual, minimum = version_tuple(version), version_tuple(exact)
    if actual is None or minimum is None:
        return None
    if constraint.startswith("^"):
        major, minor, patch = minimum
        maximum = (major + 1, 0, 0) if major else ((0, minor + 1, 0) if minor else (0, 0, patch + 1))
        return minimum <= actual < maximum
    return actual == minimum


def read_qmod(path):
    with zipfile.ZipFile(path) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())):
            raise ValueError(f"Duplicate archive entries in {path}")
        bad = archive.testzip()
        if bad:
            raise ValueError(f"Invalid CRC for {bad} in {path}")
        return json.loads(archive.read("mod.json"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qmod", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    (out / "archives").mkdir(parents=True, exist_ok=True)
    (out / "elf").mkdir(exist_ok=True)
    root = read_qmod(args.qmod)
    selected, requests, failures, binaries = {}, [], [], []
    pending = [(root["id"], dep) for dep in root.get("dependencies", [])]
    while pending:
        parent, dep = pending.pop(0)
        requests.append({"parent": parent, **dep})
        ident = dep["id"]
        if ident in selected:
            current = selected[ident]["manifest"]["version"]
            if satisfies(current, dep["version"]) is not False:
                continue
            # A manifest may point at an older compatible release than another
            # parent requires. Upgrade using that parent's explicit URL only.
            minimum = version_tuple(dep["version"].lstrip("=^"))
            if minimum is None or version_tuple(current) is None or minimum <= version_tuple(current):
                continue
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", ident):
            raise ValueError(f"Unsafe mod id: {ident}")
        url = dep.get("downloadIfMissing")
        if not url or not url.startswith("https://github.com/"):
            failures.append({"id": ident, "reason": "No supported manifest download URL", "request": dep})
            continue
        dest = out / "archives" / (ident + "-" + hashlib.sha256(url.encode()).hexdigest()[:12] + ".qmod")
        try:
            if not dest.exists():
                partial = dest.with_suffix(".partial")
                subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                                "--retry", "2", "--max-time", "90", "--output", str(partial), url], check=True)
                read_qmod(partial)
                partial.rename(dest)
            manifest = read_qmod(dest)
            if manifest["id"] != ident:
                raise ValueError(f"Manifest id {manifest['id']} differs from requested {ident}")
            entry = {"id": ident, "url": url, "sha256": hashlib.sha256(dest.read_bytes()).hexdigest(),
                     "archive": str(dest.relative_to(out)), "manifest": manifest}
            selected[ident] = entry
            binaries[:] = [binary for binary in binaries if binary["mod"] != ident]
            print(f"Fetched {ident} {manifest['version']}", flush=True)
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", manifest["version"]):
                raise ValueError("Unsafe manifest version")
            libdir = out / "elf" / ident / manifest["version"]
            libdir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(dest) as archive:
                for name in archive.namelist():
                    # No extractall: all outputs are basenames under a validated mod id.
                    if name.endswith(".so"):
                        target = libdir / Path(name).name
                        if target.exists() and target.read_bytes() != archive.read(name):
                            raise ValueError(f"Conflicting library basename: {name}")
                        target.write_bytes(archive.read(name))
                        elf = Elf(target)
                        if not elf.ok:
                            raise ValueError(f"Not an ARM64 ELF: {name}")
                        binaries.append({"mod": ident, "member": name, "file": str(target.relative_to(out)),
                                         "sha256": hashlib.sha256(elf.data).hexdigest(),
                                         "needed": elf.needed, "tls": elf.tls,
                                         "relocations": dict(elf.relocs),
                                         "imports": sorted(name for name, _ in elf.imports),
                                         "exports": sorted(elf.exports)})
            pending.extend((ident, child) for child in manifest.get("dependencies", [])
                           if child.get("required", True))
        except (subprocess.CalledProcessError, ValueError, KeyError, zipfile.BadZipFile) as error:
            failures.append({"id": ident, "url": url, "reason": str(error)})
            print(f"FAILED {ident}: {error}", flush=True)
    constraints = []
    for req in requests:
        version = selected.get(req["id"], {}).get("manifest", {}).get("version")
        constraints.append({"parent": req["parent"], "id": req["id"], "required": req["version"],
                            "selected": version, "satisfied": satisfies(version, req["version"]) if version else False})
    lock = {"root": root, "packages": list(selected.values()), "constraints": constraints,
            "failures": failures, "binaries": binaries,
            "limitations": "Static inventory only. No mod executed; symbol providers and runtime hook compatibility are unproven."}
    (out / "coremods-lock.json").write_text(json.dumps(lock, indent=2) + "\n")
    conflicts = [c for c in constraints if c["satisfied"] is not True]
    lines = ["# Supplied CoreMods: dependency audit", "", "Static analysis only; no mods installed or executed.", "",
             "| Mod | Version | Game | ELF files |", "| --- | --- | --- | --- |"]
    for ident, entry in selected.items():
        m = entry["manifest"]
        lines.append(f"| {ident} | {m['version']} | {m.get('packageVersion', 'unspecified')} | "
                     f"{sum(b['mod'] == ident for b in binaries)} |")
    lines += ["", f"{len(selected)} packages; {len(binaries)} ELF libraries; {len(failures)} download/audit failures; "
              f"{len(conflicts)} unsatisfied or unverified constraints.", "", "## Constraints requiring review", ""]
    lines += [f"- {c['parent']} needs {c['id']} {c['required']}; selected {c['selected']}" for c in conflicts]
    lines += ["", "## Native dependencies", ""]
    lines += [f"- `{b['mod']}/{Path(b['file']).name}`: {', '.join(b['needed'])}" for b in binaries]
    lines += ["", "## Loader and hook imports", ""]
    for b in binaries:
        interesting = [s for s in b["imports"] if any(w in s.lower() for w in
                       ("modloader", "flamingo", "mprotect", "mmap", "dlopen", "cache", "a64hook", "and64", "hook"))]
        if interesting:
            lines += [f"- `{b['mod']}/{Path(b['file']).name}`: " + ", ".join(f"`{s}`" for s in interesting)]
    lines += ["", "## Failures", ""]
    lines += [f"- {f['id']}: {f['reason']}" for f in failures]
    (out / "coremods-audit.md").write_text("\n".join(lines) + "\n")
    print(f"Inventory: {len(selected)} packages, {len(binaries)} ELFs, {len(failures)} failures, {len(conflicts)} constraints to review")


if __name__ == "__main__":
    main()
