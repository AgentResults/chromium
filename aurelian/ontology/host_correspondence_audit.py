#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ACM-7 audit 3 — the HOST-side correspondence audit (design section 6,
round-5 review M7).

[EMBODIMENT-REFLECTIVE-CORRESPONDENCE] is an obligation over the HOST's
self-describing structures: every enumerated Chromium structure family maps
to a projector OR carries an explicit recorded out-of-scope row, revisited
per roll. The enumeration lives HERE, pinned to host anchors (a file plus a
token that IS the family's self-describing surface): a family whose anchor
exists but whose ontology row was deleted FAILS the build (the planted-
failure RED); an anchor that vanishes flags the enumeration itself as stale.
The cdp-descriptor leg re-parses the build-generated descriptor every build,
so the audit auto-grows with every Chromium roll.
"""

import argparse
import json
import pathlib
import sys

import ontology_lib

# family -> (host anchor relative to src root, token that must appear).
# The descriptor-backed families (anchor None) are checked against the
# build-generated protocol.json passed via --descriptor.
FAMILIES = {
    "cdp-descriptor": (None, None),
    "prefs-registry": ("components/prefs/pref_service.h",
                       "IteratePreferenceValues"),
    "targets": ("content/public/browser/devtools_agent_host.h",
                "GetOrCreateAll"),
    "keyed-services-graph": ("components/keyed_service/core/"
                             "dependency_manager.h", "GetDependencyGraph()"),
    "ax-tree": (None, "Accessibility"),
    "features-flags-registry": ("base/feature_list.h", "FeatureList"),
    "mojo-interface-broker": ("third_party/blink/public/mojom/"
                              "browser_interface_broker.mojom",
                              "BrowserInterfaceBroker"),
    "command-line-switches": ("base/command_line.h", "CommandLine"),
    "histograms-registry": ("base/metrics/histogram.h", "Histogram"),
    "extensions-registry": ("extensions/browser/extension_registry.h",
                            "ExtensionRegistry"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src-root", required=True)
    parser.add_argument("--ontology", required=True)
    parser.add_argument("--descriptor", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()

    ontology = ontology_lib.parse_ontology(args.ontology)
    rows = ontology.get("correspondence", {})
    projectors = {
        row.get("projector")
        for row in ontology.get("roots", {}).values()
        if row.get("kind") == "projector"
    }

    descriptor = json.loads(
        pathlib.Path(args.descriptor).read_text(encoding="utf-8"))
    domains = {d.get("domain") for d in descriptor.get("domains", [])}

    failures = []

    # The host enumeration: every family's anchor must still exist...
    for family, (anchor, token) in FAMILIES.items():
        if anchor is None:
            if token is not None and token not in domains:
                failures.append(
                    f"STALE ENUMERATION: family '{family}' expects domain "
                    f"'{token}' in the descriptor; the host no longer has "
                    "it — revisit the enumeration")
            continue
        path = pathlib.Path(args.src_root) / anchor
        if not path.is_file():
            failures.append(
                f"STALE ENUMERATION: family '{family}' anchor {anchor} is "
                "gone from the host — revisit the enumeration")
        elif token and token not in path.read_text(encoding="utf-8"):
            failures.append(
                f"STALE ENUMERATION: family '{family}' anchor {anchor} no "
                f"longer contains '{token}' — revisit the enumeration")

    # ...and every family must carry exactly one recorded disposition.
    for family in FAMILIES:
        if family not in rows:
            failures.append(
                f"UNRECORDED FAMILY: '{family}' exists at the host but has "
                "no correspondence row — map it to a projector or record it "
                "out-of-scope with a reason")
    for family, row in rows.items():
        if family not in FAMILIES:
            failures.append(
                f"UNKNOWN FAMILY: correspondence row '{family}' is not in "
                "the host enumeration — add its anchor to "
                "host_correspondence_audit.py FAMILIES")
            continue
        status = row.get("status")
        if status in ("mapped", "covered-via"):
            if row.get("projector") not in projectors:
                failures.append(
                    f"DANGLING PROJECTOR: '{family}' maps to "
                    f"{row.get('projector')!r}, which no roots row declares")
        elif status == "out-of-scope":
            if not row.get("reason"):
                failures.append(
                    f"UNREASONED OUT-OF-SCOPE: '{family}' records no reason")
        else:
            failures.append(f"BAD STATUS: '{family}' status={status!r}")

    # The descriptor leg auto-grows per roll: the mapped projector is
    # data-driven over the WHOLE descriptor, so the only assert that can
    # rot is the descriptor itself shrinking past recognition.
    if len(domains) < 40:
        failures.append(
            f"DESCRIPTOR SHRANK: {len(domains)} domains (< 40) — the "
            "cdp-descriptor family no longer looks like the catalog the "
            "mirror was built against")

    if failures:
        print("ACM HOST-CORRESPONDENCE AUDIT FAILED (design section 6: "
              "every host self-describing structure family -> projector or "
              "recorded out-of-scope row):", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    pathlib.Path(args.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
