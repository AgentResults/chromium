#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ACM-R audit — acm-no-parallel-bespoke (design section 7, HS-2).

The mirror REPLACED the bespoke surface; this GRC-14-style lock keeps it
replaced. Scope: every .cc/.h under aurelian/handles/** (recursive), the
federation bring-up set (federation/uds_register*, federation/wss_peer*),
capability/cap_gated_cookies*, plus the renderer Mojo bridge (renderer/**)
and the virtual-A/V layer (media/**) — the two KEEP families round-6 made
the lock enforce like every other row.

Fails the build when:
  (a) an in-scope file exists with no inventory row (or with two rows);
  (b) a DELETE-row file still exists;
  (b2) a non-DELETE row names a file that does not exist (a ghost row);
  (c) a facade verb literal in handles/root/root_handle.cc (a `msg == "x"`
      comparison that is not a __-identity verb or `describe`) has no
      FOLD-verb row — a bespoke verb may not shadow the mirror unrecorded.
      (Grep-shaped on purpose, like the no-RunLoop audit: the facade
      classes compare `msg`; the root's mount dispatch compares `name`,
      which is exactly the mounted-set surface the ACM-7 coverage audit
      already three-way-locks.);
  (d) an inventory row names a path outside this audit's scope (round-5
      review M8: audit scope MUST equal inventory scope).
"""

import argparse
import pathlib
import re
import sys

# The audited extensions: control surface is carried by C++ sources.
EXTS = {".cc", ".h"}

# Recursive scope directories (aurelian/-relative).
SCOPE_DIRS = ("handles", "renderer", "media")

# Prefix-glob scope (aurelian/-relative).
SCOPE_PREFIXES = (
    "federation/uds_register",
    "federation/wss_peer",
    "capability/cap_gated_cookies",
)

DISPOSITIONS = {
    "DELETE",
    "FOLD",
    "KEEP-infrastructure",
    "KEEP-test-harness",
    "OUT-OF-SCOPE",
}

# Verb literals exempt from the FOLD requirement: identity/introspection plus
# the uniform `describe`.
VERB_EXEMPT = {"describe"}


def in_scope(rel: str) -> bool:
    p = pathlib.PurePosixPath(rel)
    if p.suffix not in EXTS:
        return False
    if p.parts and p.parts[0] in SCOPE_DIRS:
        return True
    return any(rel.startswith(prefix) for prefix in SCOPE_PREFIXES)


def enumerate_scope(aurelian_root: pathlib.Path):
    found = []
    for d in SCOPE_DIRS:
        base = aurelian_root / d
        if base.is_dir():
            for f in sorted(base.rglob("*")):
                rel = f.relative_to(aurelian_root).as_posix()
                if f.is_file() and in_scope(rel):
                    found.append(rel)
    for prefix in SCOPE_PREFIXES:
        parent = (aurelian_root / prefix).parent
        stem = pathlib.PurePosixPath(prefix).name
        if parent.is_dir():
            for f in sorted(parent.iterdir()):
                rel = f.relative_to(aurelian_root).as_posix()
                if f.is_file() and f.name.startswith(stem) and in_scope(rel):
                    found.append(rel)
    return found


def parse_inventory(path: pathlib.Path):
    """Returns (rows, fold_verbs): rows = [(files, disposition, reason)];
    fold_verbs = {facade: set(verbs)}. Table rows are |-delimited; the Rows
    table is recognized by a DISPOSITION cell, the FOLD table by its three
    columns under the '## FOLD verbs' heading."""
    rows = []
    fold_verbs = {}
    section = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if not cells or set("".join(cells)) <= {"-", " ", ":"}:
            continue  # separator row
        if section == "Rows":
            if len(cells) != 3 or cells[0] == "files":
                continue
            files = cells[0].split()
            disposition = cells[1]
            rows.append((files, disposition, cells[2]))
        elif section == "FOLD verbs":
            if len(cells) != 3 or cells[0] == "facade":
                continue
            fold_verbs[cells[0]] = set(cells[1].split())
    return rows, fold_verbs


def scan_facade_verbs(root_handle_cc: pathlib.Path):
    text = root_handle_cc.read_text(encoding="utf-8")
    verbs = set(re.findall(r'msg\s*==\s*"([^"]+)"', text))
    return {
        v for v in verbs if not v.startswith("__") and v not in VERB_EXEMPT
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--aurelian-root", required=True)
    parser.add_argument("--inventory", required=True)
    parser.add_argument("--stamp")
    args = parser.parse_args()

    aurelian_root = pathlib.Path(args.aurelian_root)
    rows, fold_verbs = parse_inventory(pathlib.Path(args.inventory))

    failures = []

    if not rows:
        failures.append("EMPTY INVENTORY: no rows parsed")

    claimed = {}
    for files, disposition, reason in rows:
        if disposition not in DISPOSITIONS:
            failures.append(
                f"BAD DISPOSITION: {disposition!r} for {' '.join(files)}")
            continue
        if not reason:
            failures.append(f"MISSING REASON: {' '.join(files)}")
        for rel in files:
            if rel in claimed:
                failures.append(f"DUPLICATE ROW: {rel}")
            claimed[rel] = disposition
            if not in_scope(rel):
                failures.append(
                    f"ROW OUTSIDE AUDIT SCOPE (d): {rel} — audit scope must "
                    "equal inventory scope")
                continue
            exists = (aurelian_root / rel).is_file()
            if disposition == "DELETE" and exists:
                failures.append(
                    f"DELETE-ROW FILE STILL EXISTS (b): {rel} — a parallel "
                    "bespoke surface survives its own disposition")
            if disposition != "DELETE" and not exists:
                failures.append(f"GHOST ROW (b2): {rel} does not exist")

    for rel in enumerate_scope(aurelian_root):
        if rel not in claimed:
            failures.append(
                f"NO INVENTORY ROW (a): {rel} — browser-control surface in "
                "no inventory category was exactly the round-2 leak")

    root_handle_cc = aurelian_root / "handles/root/root_handle.cc"
    if root_handle_cc.is_file():
        all_fold = set().union(*fold_verbs.values()) if fold_verbs else set()
        for verb in sorted(scan_facade_verbs(root_handle_cc)):
            if verb not in all_fold:
                failures.append(
                    f"UNRECORDED FACADE VERB (c): root_handle.cc serves "
                    f"'{verb}' with no FOLD-verb row — a bespoke verb may "
                    "not shadow the mirror unrecorded")
    else:
        failures.append("MISSING handles/root/root_handle.cc")

    if failures:
        print(
            "ACM NO-PARALLEL-BESPOKE AUDIT FAILED (design section 7: the "
            "mirror REPLACES the bespoke surface; every in-scope file "
            "carries exactly one executed disposition):",
            file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    if args.stamp:
        pathlib.Path(args.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
