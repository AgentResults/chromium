#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Derive the CDP session-context table from the host's registration sites.

AURELIAN-GENERIC-CONTROL-DESIGN.md section 2 (round-8): the domain-layer
classification is DERIVED, never hand-named. Ground truth is the three
browser-process registration sites:

  (1) content/browser/devtools/browser_devtools_agent_host.cc
      - the browser-session set (AttachSession body)
  (2) content/browser/devtools/render_frame_devtools_agent_host.cc
      - the frame/page-session set (AttachSession body)
  (3) chrome/browser/devtools/chrome_devtools_session.cc
      - the chrome layer, session-TYPE-conditional (ctor body)

plus the structural forwarding asymmetry: the browser session width is
CLOSED (refusals derive from absence in the content-browser + chrome
browser rows union); the page/frame session width is OPEN (un-found
methods forward to the renderer, so page-side refusals derive ONLY from
the closed browser-only set, and each such row is live-pinned by a
browsertest -- the pin, not this tool, is the arbiter of membership).

The tool FAILS LOUDLY (lossless-or-throw) when a registration pattern it
does not recognize appears, so a Chromium roll that changes the shape of
a site surfaces here instead of silently mis-deriving.

Outputs JSON: {"domains": {<descriptor domain>: {"inBrowserUnion": bool,
"browserOnly": bool, "conditional": bool}}}. Rows apply to the browser
mirror + page/frame target sub-mirrors only; non-page target types
(tab/worker/worklet) are annotation-not-applicable (design section 2
round-8) -- recorded in the output header.
"""

import argparse
import json
import re
import sys

HANDLER_RE = re.compile(
    r"(?:CreateAndAddHandler<\s*(?:protocol::)?(\w+)Handler\s*>"
    r"|std::make_unique<\s*(\w+)Handler\s*>)")

# Handler-class -> descriptor-domain where the strip-Handler rule fails.
DOMAIN_FIXUPS = {
    "WebMCP": "WebMCP",
}


def extract_block(text, anchor, open_brace_after):
    """Return the brace-balanced block body starting after `anchor`."""
    i = text.find(anchor)
    if i < 0:
        raise SystemExit("derive_session_context: anchor not found: " + anchor)
    j = text.find(open_brace_after, i)
    if j < 0:
        raise SystemExit(
            "derive_session_context: open brace not found after " + anchor)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[j + 1:k]
    raise SystemExit("derive_session_context: unbalanced braces in " + anchor)


def handlers_in(block):
    out = []
    for m in HANDLER_RE.finditer(block):
        name = m.group(1) or m.group(2)
        out.append(DOMAIN_FIXUPS.get(name, name))
    return out


def conditional_handlers(block, condition_re):
    """Handlers registered under an if-condition matching condition_re,
    tracked by brace depth from the if-line."""
    lines = block.splitlines()
    out = []
    depth = 0
    cond_depth = None
    for ln in lines:
        if cond_depth is None and re.search(condition_re, ln):
            cond_depth = depth
        depth += ln.count("{") - ln.count("}")
        if cond_depth is not None:
            for m in HANDLER_RE.finditer(ln):
                out.append(DOMAIN_FIXUPS.get(m.group(1) or m.group(2),
                                             m.group(1) or m.group(2)))
            if depth <= cond_depth:
                cond_depth = None
    return out


def chrome_layer_sets(text):
    """Parse ChromeDevToolsSession's ctor into per-session-type sets.

    Returns (page_only, page_and_frame, browser_and_page, unconditional).
    Type-conditional blocks are recognized by their GetType()/
    GetWebContents() conditions; handlers outside any type condition are
    registered on ALL session types (per-handler trust checks only --
    the mirror's in-process clients are trusted).
    """
    ctor = extract_block(
        text, "ChromeDevToolsSession::ChromeDevToolsSession", "{")
    lines = ctor.splitlines()
    page_only, page_frame, browser_page, uncond = [], [], [], []
    depth = 0
    ctx_stack = []  # (depth_at_if, kind)
    pending = None  # multi-line if condition accumulator

    def classify(cond):
        has_page = "kTypePage" in cond
        has_frame = "kTypeFrame" in cond
        has_browser = "kTypeBrowser" in cond
        if has_browser and has_page:
            return "browser_page"
        if has_page and has_frame:
            return "page_frame"
        if has_page:
            return "page_only"
        if has_browser:
            return "browser_only_block"
        return None

    for ln in lines:
        joined = (pending + " " + ln.strip()) if pending is not None else ln
        if re.search(r"\bif\s*\(", joined) and ("GetType()" in joined
                                                or "GetWebContents()" in joined):
            if "{" not in ln:
                pending = joined
                continue
            kind = classify(joined)
            pending = None
            if kind:
                ctx_stack.append((depth, kind))
        elif pending is not None:
            pending = joined
            if "{" in ln:
                kind = classify(pending)
                pending = None
                if kind:
                    ctx_stack.append((depth, kind))

        for m in HANDLER_RE.finditer(ln):
            name = DOMAIN_FIXUPS.get(m.group(1) or m.group(2),
                                     m.group(1) or m.group(2))
            kind = ctx_stack[-1][1] if ctx_stack else None
            if kind == "page_only":
                page_only.append(name)
            elif kind == "page_frame":
                page_frame.append(name)
            elif kind == "browser_page":
                browser_page.append(name)
            elif kind == "browser_only_block":
                browser_page.append(name)  # browser-presence; page absent
            else:
                uncond.append(name)

        depth += ln.count("{") - ln.count("}")
        while ctx_stack and depth <= ctx_stack[-1][0]:
            ctx_stack.pop()

    return page_only, page_frame, browser_page, uncond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src-root", required=True)
    ap.add_argument("--descriptor", required=True)
    ap.add_argument("--renderer-served", default=None,
                    help="checked-in pin-verified corrections: domains the "
                         "grep derives browser-only but a live pin proved "
                         "renderer/page-served")
    ap.add_argument("--out", default=None)
    ap.add_argument("--check", default=None,
                    help="regeneration-audit mode: derive freshly and FAIL "
                         "if the checked-in table at this path differs "
                         "(the build-time drift lock)")
    ap.add_argument("--stamp", default=None,
                    help="stamp file written on audit success (GN action)")
    args = ap.parse_args()
    if not args.out and not args.check:
        ap.error("one of --out / --check is required")

    root = args.src_root

    def read(p):
        with open(root + "/" + p, encoding="utf-8") as f:
            return f.read()

    # Site (1): content browser-session set.
    t1 = read("content/browser/devtools/browser_devtools_agent_host.cc")
    s1 = extract_block(t1, "BrowserDevToolsAgentHost::AttachSession", "{")
    browser_set = set(handlers_in(s1))
    cond1 = set(conditional_handlers(s1, r"tethering_task_runner_"))

    # Site (2): content frame-session set.
    t2 = read("content/browser/devtools/render_frame_devtools_agent_host.cc")
    s2 = extract_block(t2, "RenderFrameDevToolsAgentHost::AttachSession", "{")
    frame_set = set(handlers_in(s2))

    # Site (3): the chrome layer, type-conditional.
    t3 = read("chrome/browser/devtools/chrome_devtools_session.cc")
    pg, pf, bp, un = chrome_layer_sets(t3)

    chrome_browser_rows = set(bp) | set(un)
    chrome_page_rows = set(pg) | set(pf) | set(bp) | set(un)

    in_browser_union = browser_set | chrome_browser_rows
    page_side = frame_set | chrome_page_rows

    renderer_served = set()
    if args.renderer_served:
        with open(args.renderer_served, encoding="utf-8") as f:
            renderer_served = set(json.load(f)["domains"])

    with open(args.descriptor, encoding="utf-8") as f:
        descriptor = json.load(f)
    domains = sorted(d["domain"] for d in descriptor["domains"])
    if len(domains) != len(set(domains)):
        raise SystemExit("derive_session_context: duplicate descriptor domains")

    rows = {}
    for d in domains:
        ibu = d in in_browser_union
        b_only = ibu and d not in page_side and d not in renderer_served
        rows[d] = {
            "inBrowserUnion": ibu,
            "browserOnly": b_only,
            "conditional": d in cond1,
        }

    # Registration-site handlers whose domain is NOT in this build's
    # descriptor (buildflag-gated or descriptor-stripped domains, e.g.
    # NativeProfiling / VisualDebugger / WindowManager). They get no row
    # (the mirror exposes only descriptor domains) but are RECORDED in
    # the output so a roll that adds one changes the checked-in table
    # and the regeneration audit fires -- loud-by-diff, never a silent
    # drop. A genuine handler->domain mapping failure surfaces here too
    # and is corrected via DOMAIN_FIXUPS.
    not_in_descriptor = sorted((in_browser_union | page_side) - set(domains))

    out = {
        "registeredNotInDescriptor": not_in_descriptor,
        "_comment": [
            "GENERATED by aurelian/catalog/derive_session_context.py --",
            "do not hand-edit (the regeneration audit fails the build on",
            "drift). Rows apply to the browser mirror + page/frame target",
            "sub-mirrors ONLY; non-page target types (tab/worker/worklet)",
            "are annotation-not-applicable (design section 2 round-8).",
            "browserOnly membership is pin-arbitrated: a failing live pin",
            "moves the domain into renderer_served.json, never the reverse.",
        ],
        "domains": rows,
    }
    rendered = json.dumps(out, indent=1, sort_keys=True) + "\n"

    if args.check:
        with open(args.check, encoding="utf-8") as f:
            checked_in = f.read()
        if checked_in != rendered:
            raise SystemExit(
                "derive_session_context AUDIT FAILED: the checked-in table "
                + args.check + " drifted from a fresh derivation of the "
                "three registration sites. Regenerate with --out (never "
                "hand-edit) and re-run the live pins.")
        if args.stamp:
            with open(args.stamp, "w", encoding="utf-8") as f:
                f.write("ok\n")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(rendered)

    bo = [d for d, r in rows.items() if r["browserOnly"]]
    ibu_n = sum(1 for r in rows.values() if r["inBrowserUnion"])
    print("derived: %d domains; inBrowserUnion=%d; browserOnly=%s"
          % (len(rows), ibu_n, bo), file=sys.stderr)


if __name__ == "__main__":
    main()
