#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ACM-7 audit 2 — acm-surface-coverage (design section 6).

Every name the sealed root mounts must be owned by a projector or carry a
recorded facade row in the authored ontology, and the three sources of the
mounted set — the root dispatch (root_handle.cc), the EmbodimentPolicy rows
(embodiment_policy.cc FullStandalone), and the ontology roots section —
must agree exactly. The generated wiring map is drift-checked against a
fresh render (regenerate with --write-map).
"""

import argparse
import pathlib
import sys

import ontology_lib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src-root", required=True)
    parser.add_argument("--ontology", required=True)
    parser.add_argument("--map")
    parser.add_argument("--write-map")
    parser.add_argument("--stamp")
    args = parser.parse_args()

    ontology = ontology_lib.parse_ontology(args.ontology)
    roots = ontology.get("roots", {})
    mounted = ontology_lib.parse_mounted_roots(args.src_root)
    policy = ontology_lib.parse_policy_roots(args.src_root)
    stamps = ontology_lib.scan_stamps(
        args.src_root, ontology_lib.production_files(ontology))

    if args.write_map:
        pathlib.Path(args.write_map).write_text(
            ontology_lib.render_wiring_map(ontology, stamps),
            encoding="utf-8")
        print(f"wrote {args.write_map}")
        return 0

    failures = []
    for name in mounted:
        if name not in roots:
            failures.append(
                f"UNOWNED MOUNT: the sealed root mounts '{name}' "
                "(root_handle.cc) but the ontology has no roots row for it")
    for name in policy:
        if name not in roots:
            failures.append(
                f"UNOWNED POLICY ROW: EmbodimentPolicy::FullStandalone grants "
                f"'{name}' but the ontology has no roots row for it")
    for name, row in roots.items():
        if name not in mounted:
            failures.append(
                f"STALE ROOT ROW: ontology row '{name}' is not mounted by "
                "root_handle.cc")
        if name not in policy:
            failures.append(
                f"STALE ROOT ROW: ontology row '{name}' has no "
                "EmbodimentPolicy::FullStandalone row")
        kind = row.get("kind")
        if kind == "projector":
            for field in ("projector", "factory", "host_structure"):
                if not row.get(field):
                    failures.append(
                        f"INCOMPLETE PROJECTOR ROW: '{name}' lacks {field}")
        elif kind == "facade":
            if not row.get("disposition"):
                failures.append(
                    f"INCOMPLETE FACADE ROW: '{name}' lacks a disposition")
        else:
            failures.append(f"BAD ROOT KIND: '{name}' kind={kind!r}")
        if not row.get("files"):
            failures.append(f"INCOMPLETE ROOT ROW: '{name}' names no files")

    if args.map and not failures:
        fresh = ontology_lib.render_wiring_map(ontology, stamps)
        checked_in = pathlib.Path(args.map).read_text(encoding="utf-8")
        if fresh != checked_in:
            failures.append(
                f"WIRING-MAP DRIFT: {args.map} no longer matches a fresh "
                "render — regenerate with --write-map")

    if failures:
        print("ACM SURFACE-COVERAGE AUDIT FAILED (design section 6: every "
              "mounted root owned by a projector or a recorded facade row):",
              file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    pathlib.Path(args.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
