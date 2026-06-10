#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""ACM-2 grep-assert (AURELIAN-GENERIC-CONTROL-TDD-PLAN): no base::RunLoop
in the HS-1 layer.

The shape HS-1 replaces nested a UI loop per dispatch (devtools_handle.cc).
The new layer — the session layer, the completion bridge, and the bridge
glue — must never reintroduce one: settlement is event-signaled, waits live
on serve threads only. This audit fails the BUILD if any audited file
mentions the banned spellings, so the invariant cannot rot silently.
"""

import argparse
import pathlib
import sys

BANNED = ("RunLoop", "run_loop")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp", required=True)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()

    failures = []
    for name in args.files:
        path = pathlib.Path(name)
        for lineno, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1):
            for banned in BANNED:
                if banned in line:
                    failures.append(f"{name}:{lineno}: {line.strip()}")

    if failures:
        print("HS-1 NO-RUNLOOP AUDIT FAILED (design section 3: the async "
              "session layer never nests a UI loop):", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    pathlib.Path(args.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
