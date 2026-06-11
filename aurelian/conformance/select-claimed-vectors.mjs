#!/usr/bin/env node
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// select-claimed-vectors.mjs — CF-V Step 1 (conformant-federation design
// §3.3a-D4/D4a): the MECHANICAL lane selection + the branch-3 classifier.
// Glue like the launcher — no protocol logic.
//
// TWO inputs:
//   Input A — the banked __handshake capture (JSON: {wireFacets:[],
//             internalFacets:[]}; the SAME artefact §3.5's pre-flight
//             checks, so selection and manifest cannot disagree).
//   Input B — aurelian-claims-draft.yaml (the drafted column block: claimed
//             ∪ queued ∪ not-claimed ∪ out-of-scope rows, the per-row
//             disposition exclusions, the CLOSED no-row prefix list — as
//             data).
//
// Classifies EVERY corpus `sub-facet:` entry into exactly one category —
// claimed / queued / not-claimed / out-of-scope / disposition-excluded /
// no-row-prefix — and EXITS NON-ZERO printing any facet in no category
// (the rowless-facet BLOCKER, closure branch 3: fails the selection step
// itself, never degrades to manual table re-review). Asserts A ⊆ B's
// claimed rows (the §3.5(1) drift BLOCKER, caught here first).
//
// Selection (D4): every vector whose facet is CLAIMED, minus the per-row
// disposition exclusions (the conduits/v0.6 eleven — printed as their own
// category), minus the PAIR-GUARD (any selected vector whose text carries
// establishHandshake/establishFederation → excluded to CF-L2, named).
// Copies the selection into temp lane dirs (relative paths preserved) and
// prints the classifier table + the full selection + every exclusion.
//
// Usage:
//   node select-claimed-vectors.mjs \
//     --handshake <captureA.json> --claims <aurelian-claims-draft.yaml> \
//     --vectors <conformance>/vectors --out /tmp/cf-v-lane
//   (prints lane dirs, one per line, to --out/LANES.txt)

import fs from 'node:fs';
import path from 'node:path';

function arg(name, dflt) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 ? process.argv[i + 1] : dflt;
}
const handshakePath = arg('handshake');
const claimsPath = arg('claims');
const vectorsRoot = arg('vectors');
const outRoot = arg('out', '/tmp/cf-v-lane');
if (!handshakePath || !claimsPath || !vectorsRoot) {
  console.error('usage: --handshake A.json --claims B.yaml --vectors dir [--out dir]');
  process.exit(64);
}

// ---- Input B: the drafted column block (strict regex parse over our own
// constrained YAML shape — the ontology_lib strict-subset precedent).
const B = fs.readFileSync(claimsPath, 'utf8');
function section(name, next) {
  const re = new RegExp(`^${name}:\\n([\\s\\S]*?)(?=^${next}|$(?![\\s\\S]))`, 'm');
  const m = B.match(re);
  return m ? m[1] : '';
}
const claimedWire = [...section('claimed-wire', '# === CLAIMED \\(internal')
  .matchAll(/^  - ([\w./*_-]+)/gm)].map(m => m[1]);
const claimedInternal = [...section('claimed-internal', '# === QUEUED')
  .matchAll(/^  - ([\w./*_-]+)/gm)].map(m => m[1]);
const facetsOf = s => [...s.matchAll(/facet: "?([\w./*_-]+)"?/g)].map(m => m[1]);
const queued = facetsOf(section('queued', '# === NOT CLAIMED'));
const notClaimed = facetsOf(section('not-claimed', '# === OUT-OF-SCOPE'));
const outOfScope = facetsOf(section('out-of-scope', '# === PER-ROW'));
const dispSec = section('disposition-exclusions', '# === THE CLOSED');
const dispDirs = [...dispSec.matchAll(/vectors-dir: ([\w./-]+)/g)].map(m => m[1]);
const nrSec = B.split('no-row-prefixes:')[1] || '';
const nrPrefixes = [...(nrSec.split('bare:')[0] || '')
  .matchAll(/- ([\w.-]+)/g)].map(m => m[1]);
const nrBare = [...(nrSec.split('bare:')[1] || '')
  .matchAll(/- ([\w.-]+)/g)].map(m => m[1]);

const PREFIX = 'legion://facets/';
const cat = new Map();   // exact facet -> category
const globs = [];        // [suffix-glob, category]
for (const [list, c] of [[claimedWire, 'claimed'], [claimedInternal, 'claimed'],
                         [queued, 'queued'], [notClaimed, 'not-claimed'],
                         [outOfScope, 'out-of-scope']]) {
  for (const f of list) {
    if (f.endsWith('/*')) globs.push([f.slice(0, -2), c]);
    else cat.set(f, c);
  }
}
const claimedSet = new Set([...claimedWire, ...claimedInternal]
  .filter(f => !f.endsWith('/*')));

// ---- Input A ⊆ B's claimed rows (the §3.5(1) drift check).
const A = JSON.parse(fs.readFileSync(handshakePath, 'utf8'));
const aFacets = [...(A.wireFacets || []), ...(A.internalFacets || [])]
  .map(u => u.startsWith(PREFIX) ? u.slice(PREFIX.length) : u);
const aNotInB = aFacets.filter(f => !claimedSet.has(f));
if (aNotInB.length) {
  console.error('DRIFT BLOCKER (§3.5(1)): manifest facets with no claimed row:');
  for (const f of aNotInB) console.error('  ' + f);
  process.exit(2);
}

// ---- The census: every (file, sub-facet) entry under vectors/.
function* walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(full);
    else if (e.isFile() && e.name.endsWith('.yaml')) yield full;
  }
}
const entries = [];  // {file, rel, facet, text}
for (const file of walk(vectorsRoot)) {
  const text = fs.readFileSync(file, 'utf8');
  for (const m of text.matchAll(/^sub-facet:\s*"?(legion:\/\/facets\/[^"\n]+?)"?\s*$/gm)) {
    entries.push({ file, rel: path.relative(vectorsRoot, file),
                   facet: m[1].slice(PREFIX.length), text });
  }
}
const distinct = new Set(entries.map(e => e.facet));
console.log(`census: ${entries.length} sub-facet entries, ${distinct.size} distinct facets`);

// ---- Classify (every entry exactly one category; rowless = BLOCKER).
function inDispDir(rel) {
  return dispDirs.some(d => rel.startsWith(d + '/') ||
                            rel.includes('/' + d + '/'));
}
function classify(facet, rel) {
  let base = null;
  if (cat.has(facet)) base = cat.get(facet);
  if (!base) {
    for (const [g, c] of globs) {
      if (facet.startsWith(g + '/')) { base = c; break; }
    }
  }
  if (!base) {
    for (const p of nrPrefixes) {
      if (facet.startsWith(p + '/')) { base = 'no-row-prefix'; break; }
    }
  }
  if (!base && nrBare.includes(facet)) base = 'no-row-prefix';
  // D4 third minus-term — but D1 governs: the disposition excludes ONLY
  // entries ROUTING a claimed facet inside the named tree (conduits/v0.6's
  // eleven floor-URI vectors); a co-located entry routing another facet
  // keeps its own facet-derived category (the realms/* three).
  if (base === 'claimed' && inDispDir(rel)) return 'disposition-excluded';
  return base;
}
const tally = {};
const rowless = [];
const classified = [];
for (const e of entries) {
  const c = classify(e.facet, e.rel);
  if (!c) { rowless.push(e); continue; }
  tally[c] = (tally[c] || 0) + 1;
  classified.push({ ...e, category: c });
}
console.log('classifier:', JSON.stringify(tally));
if (rowless.length) {
  console.error('ROWLESS-FACET BLOCKER (closure branch 3) — facets in NO category:');
  for (const e of rowless) console.error(`  ${e.facet}  (${e.rel})`);
  process.exit(3);
}
console.log(`all ${entries.length} entries classified — remainder ∅`);

// ---- The IMPL-DRIVING predicate (the CF-V amendment, design §3.3a-D5):
// vector evidence binds to vectors whose ops the canonical runner DRIVES
// AGAINST THE SPAWNED PEER. Derived from op-block-runner.js's own op
// dispatch (runOp, lib/op-block-runner.js:231-361): a `call` is dispatchable
// iff it is a "{ref}.method" handle/fixture call (REF_RE_DOT :364 — routed
// to the spawned peer's session/slots), a `test.*` op (:356-358 — "each op
// is an ask against the spawned peer subprocess"), or one of the bare ops
// in the :246-353 switch — every non-local one of which launches or asks
// the AGENTSPACES_PEER_CMD peer (Peer.new/Embodiment.new → launchPeer
// :339; establishHandshake → launchPeerPair :271; rm.get/TestHandleClass.
// define/Peer.getResourceManifest → session asks; actAs/TempFile.write are
// pure local glue). Anything else throws `unsupported op` (:361) — those
// vectors are HARNESS-VOCABULARY reference programs the runner never
// implemented (the recorded upstream gap, design §9): their facets bind to
// SCENARIO/PROBE evidence instead.
const BARE_OPS = new Set([
  'rm.get', 'actAs', 'establishHandshake', 'TestHandleClass.define',
  'Peer.getResourceManifest', 'TempFile.write', 'Embodiment.new', 'Peer.new',
]);
function opCalls(text) {
  return [...text.matchAll(/^\s*(?:- )?call:\s*"?([^"\n]+?)"?\s*$/gm)]
    .map(m => m[1].trim());
}
function implDriving(text) {
  const calls = opCalls(text);
  if (!calls.length) return false;  // no op stream at all
  return calls.every(c => /^\{[^}]+\}\./.test(c) || c.startsWith('test.') ||
                          BARE_OPS.has(c));
}
function opBlockForm(text) {
  // The runner only loads vectors with input + expected (loadVectors).
  return /^input:/m.test(text) && /^expected:/m.test(text);
}

// ---- Selection: claimed entries, minus pair-guard, minus harness-
// vocabulary, minus non-op-block-form; disposition exclusions already
// classified out above. Every exclusion printed with its category.
const PAIR = /establishHandshake|establishFederation/;
const selected = [];
const pairExcluded = [];
const harnessVocab = [];
const nonOpBlock = [];
const seenFiles = new Set();
for (const e of classified) {
  if (e.category !== 'claimed') continue;
  if (seenFiles.has(e.rel)) continue;  // one copy per file
  seenFiles.add(e.rel);
  if (PAIR.test(e.text)) pairExcluded.push(e);
  else if (!opBlockForm(e.text)) nonOpBlock.push(e);
  else if (!implDriving(e.text)) harnessVocab.push(e);
  else selected.push(e);
}
console.log(`selection: ${selected.length} IMPL-DRIVING claimed vectors ` +
            `(pair-guard ${pairExcluded.length} -> CF-L2; ` +
            `harness-vocabulary ${harnessVocab.length} -> scenario/probe ` +
            `evidence (the §9 upstream runner gap); ` +
            `non-op-block-form ${nonOpBlock.length}; ` +
            `disposition-excluded ${tally['disposition-excluded'] || 0})`);
for (const e of pairExcluded) console.log(`  PAIR-GUARD -> CF-L2: ${e.rel}`);
for (const e of harnessVocab) {
  const bad = opCalls(e.text).filter(
      c => !/^\{[^}]+\}\./.test(c) && !c.startsWith('test.') &&
           !BARE_OPS.has(c));
  console.log(`  HARNESS-VOCABULARY: ${e.rel}  [${e.facet}]  ` +
              `ops: ${[...new Set(bad)].join(', ')}`);
}
for (const e of nonOpBlock) {
  console.log(`  NON-OP-BLOCK-FORM: ${e.rel}  [${e.facet}]`);
}

// ---- The lane pin (Input B carries the expected impl-driving count as
// DATA): a harness-internal vector misclassified as impl-driving moves the
// count — exit non-zero. The mutation RED flips one vector's category in a
// corpus COPY (the corpus itself is never edited).
const pinMatch = B.match(/^expected-lane:\s*\{\s*impl-driving:\s*(\d+)\s*\}/m);
if (pinMatch) {
  const pin = Number(pinMatch[1]);
  if (pin !== selected.length) {
    console.error(`LANE-PIN MISMATCH: expected impl-driving=${pin}, ` +
                  `computed ${selected.length} — a vector changed category ` +
                  `(or the corpus moved); re-derive the table (§3.5(4)).`);
    process.exit(4);
  }
  console.log(`lane pin: impl-driving=${pin} ✓`);
}

// ---- Copy into temp lanes (relative paths preserved).
fs.rmSync(outRoot, { recursive: true, force: true });
const laneDirs = new Set();
for (const e of selected) {
  const dst = path.join(outRoot, e.rel);
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.copyFileSync(e.file, dst);
  laneDirs.add(path.dirname(dst));
}
const lanes = [...laneDirs].sort();
fs.writeFileSync(path.join(outRoot, 'LANES.txt'), lanes.join('\n') + '\n');
fs.writeFileSync(path.join(outRoot, 'SELECTION.json'), JSON.stringify({
  census: { entries: entries.length, distinct: distinct.size },
  tally, selected: selected.map(e => ({ rel: e.rel, facet: e.facet })),
  pairExcluded: pairExcluded.map(e => e.rel),
  harnessVocabulary: harnessVocab.map(e => ({ rel: e.rel, facet: e.facet })),
  nonOpBlockForm: nonOpBlock.map(e => ({ rel: e.rel, facet: e.facet })),
}, null, 2));
console.log(`lanes: ${lanes.length} dirs under ${outRoot} (LANES.txt, SELECTION.json)`);
