#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Shared machinery for the ACM-7 ontology audits (design section 6).

Parses the authored source-domain ontology (a strict two-space-indent YAML
subset — the GN action environment is plain python3, no pyyaml), scans
production sources for legion://types/ stamps, parses the sealed root's
mounted set and the EmbodimentPolicy rows, and renders the generated wiring
map. The three audit scripts (consistency / surface-coverage / host
correspondence) are thin checks over these primitives.
"""

import pathlib
import re

STAMP_RE = re.compile(r"legion://types/[A-Za-z0-9_]+")
MOUNT_RE = re.compile(r'name == "([a-z_]+)"')
QUOTED_RE = re.compile(r'"([a-z_]+)"')


class OntologyError(Exception):
    """A malformed ontology file (parse-level, not audit-level)."""


def parse_ontology(path):
    """Parse the .ontology.yaml subset into nested dicts/lists/scalars.

    Subset rules (exactly what the authored file uses):
      - comments (#...) and blank lines ignored;
      - `key:` opens a nested map (two-space child indent);
      - `key: value` is a scalar entry;
      - `- item` appends to a list under the opening key;
      - keys may contain URIs (the trailing-colon rule disambiguates).
    """
    root = {}
    # Stack of (indent, container) — containers are dicts; lists are
    # materialized when the first `- ` child arrives.
    stack = [(-1, root)]
    pending_key = None  # (indent, dict, key) awaiting nested content.

    for lineno, raw in enumerate(
            pathlib.Path(path).read_text(encoding="utf-8").splitlines(),
            start=1):
        stripped = raw.split("#", 1)[0].rstrip() if raw.lstrip().startswith(
            "#") else raw.rstrip()
        if not stripped.strip():
            continue
        indent = len(stripped) - len(stripped.lstrip())
        line = stripped.strip()

        # Resolve the pending nested key against this line's indent.
        if pending_key is not None:
            p_indent, p_dict, p_key = pending_key
            if indent > p_indent:
                container = [] if line.startswith("- ") else {}
                p_dict[p_key] = container
                stack.append((p_indent, container))
            else:
                p_dict[p_key] = {}
            pending_key = None

        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack:
            raise OntologyError(f"{path}:{lineno}: indentation underflow")
        container = stack[-1][1]

        if line.startswith("- "):
            if not isinstance(container, list):
                raise OntologyError(
                    f"{path}:{lineno}: list item outside a list")
            container.append(line[2:].strip())
            continue
        if not isinstance(container, dict):
            raise OntologyError(f"{path}:{lineno}: map entry inside a list")
        if line.endswith(":"):
            key = line[:-1].strip()
            pending_key = (indent, container, key)
            continue
        sep = line.find(": ")
        if sep < 0:
            raise OntologyError(f"{path}:{lineno}: unparseable line: {line}")
        container[line[:sep].strip()] = line[sep + 2:].strip()

    if pending_key is not None:
        p_indent, p_dict, p_key = pending_key
        p_dict[p_key] = {}
    return root


def production_files(ontology):
    """The union of every root row's files — the consistency-scan scope."""
    files = []
    for name, row in ontology.get("roots", {}).items():
        for f in row.get("files", []):
            if f not in files:
                files.append(f)
    return files


def scan_stamps(src_root, rel_files):
    """legion://types/ literals per production file -> {type_uri: [files]}."""
    stamps = {}
    for rel in rel_files:
        if rel.endswith(("_browsertest.cc", "_unittest.cc")):
            continue  # tests assert stamps; they do not make them
        path = pathlib.Path(src_root) / "aurelian" / rel
        if not path.is_file():
            raise OntologyError(f"ontology names a missing file: {rel}")
        for uri in STAMP_RE.findall(path.read_text(encoding="utf-8")):
            stamps.setdefault(uri, [])
            if rel not in stamps[uri]:
                stamps[uri].append(rel)
    return stamps


def parse_mounted_roots(src_root):
    """The sealed root's mount dispatch — every `name == "x"` literal."""
    path = (pathlib.Path(src_root) / "aurelian" / "handles" / "root" /
            "root_handle.cc")
    return sorted(set(MOUNT_RE.findall(path.read_text(encoding="utf-8"))))


def parse_policy_roots(src_root):
    """EmbodimentPolicy::FullStandalone()'s quoted capability rows."""
    path = (pathlib.Path(src_root) / "aurelian" / "membrane" /
            "embodiment_policy.cc")
    text = path.read_text(encoding="utf-8")
    start = text.find("EmbodimentPolicy::FullStandalone()")
    if start < 0:
        raise OntologyError("FullStandalone() not found in embodiment_policy.cc")
    call = text.find("WithCapabilities(", start)
    end = text.find(");", call)
    if call < 0 or end < 0:
        raise OntologyError("WithCapabilities row list not found")
    return sorted(set(QUOTED_RE.findall(text[call:end])))


def render_wiring_map(ontology, stamps):
    """The generated wiring map — root -> projector -> types -> host family."""
    types = ontology.get("types", {})
    corr = ontology.get("correspondence", {})
    by_file = {}
    for uri, row in types.items():
        by_file.setdefault(row.get("stamped_by", "?"), []).append(uri)

    lines = [
        "# Aurelian wiring map (GENERATED — do not edit)",
        "",
        "Regenerate: `python3 aurelian/ontology/surface_coverage_audit.py "
        "--src-root . --ontology aurelian/ontology/"
        "aurelian-source-domain.ontology.yaml --write-map "
        "aurelian/ontology/AURELIAN-WIRING-MAP.md`",
        "",
        "## Mounted roots (legion://chrome/<name>)",
        "",
        "| root | kind | projector | factory | host structure | "
        "types stamped | files |",
        "|---|---|---|---|---|---|---|",
    ]
    for name, row in ontology.get("roots", {}).items():
        kind = row.get("kind", "?")
        stamped = sorted(
            uri for uri, srcs in stamps.items()
            if any(f in row.get("files", []) for f in srcs))
        lines.append("| {} | {} | {} | {} | {} | {} | {} |".format(
            name, kind,
            row.get("projector", "—"),
            row.get("factory", "—"),
            row.get("host_structure", "—"),
            "<br>".join(stamped) if stamped else "—",
            "<br>".join(row.get("files", []))))
    lines += [
        "",
        "## Layer-1 types -> IDENTIFIES -> Layer-3 canonical",
        "",
        "| type | identifies | stamped by |",
        "|---|---|---|",
    ]
    for uri, row in types.items():
        lines.append("| {} | {} | {} |".format(
            uri, row.get("identifies", "?"), row.get("stamped_by", "?")))
    lines += [
        "",
        "## Host correspondence (design section 6 — host-side)",
        "",
        "| family | status | projector | reason/note |",
        "|---|---|---|---|",
    ]
    for fam, row in corr.items():
        lines.append("| {} | {} | {} | {} |".format(
            fam, row.get("status", "?"), row.get("projector", "—"),
            row.get("reason", row.get("note", "—"))))
    lines.append("")
    return "\n".join(lines)
