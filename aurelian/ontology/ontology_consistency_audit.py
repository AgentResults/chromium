#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ACM-7 audit 1 — acm-ontology-consistency (design section 6).

Every legion://types/ stamp in the projector/facade production sources must
be declared in the authored ontology, every declared type must IDENTIFIES a
declared canonical target, and — the Phase-A review F2 discipline made
permanent — every declared type must actually be stamped (the authored set
derives from the code, never from a doc list verbatim). Build-time, so the
binding cannot rot silently.
"""

import argparse
import pathlib
import sys

import ontology_lib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src-root", required=True)
    parser.add_argument("--ontology", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()

    ontology = ontology_lib.parse_ontology(args.ontology)
    declared = ontology.get("types", {})
    canonical = set(ontology.get("canonical", []))
    scan_scope = ontology_lib.production_files(ontology)
    stamps = ontology_lib.scan_stamps(args.src_root, scan_scope)

    failures = []
    for uri, files in sorted(stamps.items()):
        if uri not in declared:
            failures.append(
                f"UNDECLARED STAMP: {uri} (stamped in {', '.join(files)}) "
                "has no row in the authored ontology")
    for uri, row in declared.items():
        identifies = row.get("identifies")
        if identifies not in canonical:
            failures.append(
                f"BAD IDENTIFIES: {uri} -> {identifies!r} is not a declared "
                "Layer-3 canonical target")
        if uri not in stamps:
            failures.append(
                f"DECLARED BUT NEVER STAMPED: {uri} — the authored set "
                "derives from what the code stamps (Phase-A review F2); "
                "remove the row or ship the projector that stamps it")
        elif row.get("stamped_by") not in stamps[uri]:
            failures.append(
                f"STAMPED_BY DRIFT: {uri} claims {row.get('stamped_by')!r} "
                f"but the scan found it in {', '.join(stamps[uri])}")

    if failures:
        print("ACM ONTOLOGY-CONSISTENCY AUDIT FAILED (design section 6: "
              "every projector stamp declared, every declared type stamped "
              "and IDENTIFIES a canonical target):", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    pathlib.Path(args.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
