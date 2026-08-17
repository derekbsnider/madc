#!/usr/bin/env python3
"""Slim claude_status.json: move historical bulk to docs/archived/.

Keeps the canonical snapshot lean (the file is loaded into agent context
every session — size is a real cost). Archived verbatim, never deleted:
  - every `recent_fixes_*` array
  - the `head` tail from its first " PREVIOUS:" marker
  - the long `mir_fork` narrative (a concise pin line stays)

Usage: scripts/slim_claude_status.py   (idempotent; run from repo root)
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATUS = os.path.join(ROOT, "claude_status.json")
ARCHIVE = os.path.join(ROOT, "docs", "archived", "claude_status_archive.json")


def main():
    with open(STATUS) as f:
        status = json.load(f)

    archive = {}
    if os.path.exists(ARCHIVE):
        with open(ARCHIVE) as f:
            archive = json.load(f)

    date = status.get("date", "unknown")
    moved = []

    def stash(key, value):
        archive.setdefault(date, {})[key] = value
        moved.append(key)

    # 1. recent_fixes_* arrays -> archive wholesale.
    for key in [k for k in list(status) if k.startswith("recent_fixes")]:
        stash(key, status.pop(key))

    # 2. head: keep the CURRENT paragraph, archive the PREVIOUS tail.
    head = status.get("head", "")
    cut = head.find(" PREVIOUS:")
    if cut > 0:
        stash("head_previous_tail", head[cut:])
        status["head"] = head[:cut]

    # 3. mir_fork: keep a concise pin line, archive the narrative.
    mf = status.get("mir_fork", "")
    if len(mf) > 600:
        stash("mir_fork_narrative", mf)
        status["mir_fork"] = (
            "MIR lives IN the repo at third_party/mir (subtree migration "
            "2026-08-11; no pin, no fork lockstep — Git versions madc and "
            "MIR together). It carries native _Complex, cleanup attribute, "
            "scope-depth layout fix, <=16-byte SIMD "
            "(vector_size/ext_vector_type), the SysV varargs/_Alignas ABI "
            "fixes, and the Mach-O writer the CIR backend depends on. "
            "github.com/derekbsnider/mir is historical + upstream-PR "
            "transport only. Full capability narrative: "
            "docs/archived/claude_status_archive.json."
        )

    note = status.get("snapshot_note", "")
    marker = "Historical bulk lives in docs/archived/claude_status_archive.json"
    if marker not in note:
        status["snapshot_note"] = (
            "Canonical current snapshot, kept LEAN (it loads into agent "
            "context every session). " + marker + " (per-date keys; "
            "appended by scripts/slim_claude_status.py). Deep session "
            "history: CHANGELOG.md, docs/test-status.md, the handoff plans, "
            "the madc-knowledge graph."
        )

    os.makedirs(os.path.dirname(ARCHIVE), exist_ok=True)
    with open(ARCHIVE, "w") as f:
        json.dump(archive, f, indent=1)
        f.write("\n")
    with open(STATUS, "w") as f:
        json.dump(status, f, indent=1, ensure_ascii=False)
        f.write("\n")

    before = sum(len(json.dumps(v)) for v in archive.get(date, {}).values())
    after = os.path.getsize(STATUS)
    print("archived keys:", ", ".join(moved) if moved else "(none — already lean)")
    print("claude_status.json now %d bytes; archived %d bytes to %s"
          % (after, before, os.path.relpath(ARCHIVE, ROOT)))


if __name__ == "__main__":
    sys.exit(main())
