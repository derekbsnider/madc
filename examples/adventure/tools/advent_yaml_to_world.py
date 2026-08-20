#!/usr/bin/env python3
# advent_yaml_to_world.py — convert open-adventure's adventure.yaml (the
# 430-point Adventure 2.5 dungeon, BSD-2, Crowther & Woods / ESR) into the
# madc hub's tagged-text .world format (include/madcdis/world_text.h).
#
# Plan: docs/plans/2026-08-20-adventure-430-plan.md (slice A4).
#
# Output model (ECS over the hub — every YAML tag keeps its name):
#   %entity <LOC_*>     long/short/conditions/sound/loud/hints/travel
#                       (travel = ordered rule array: {verbs, cond, action,
#                        nodwarves}; cond = null | [pct,N] | [carry,OBJ] |
#                        [with,OBJ] | [not,OBJ,STATE])
#   %entity <object>    words/inventory/states/descriptions/changes/sounds/
#                       texts/immovable/treasure/fixed_at
#   %link <object> in <LOC>          initial placement (primary location)
#   %entity adv-vocabulary   motions/actions/objwords: word -> [IDS...]
#   %entity adv-action-defaults      ACTION -> default message text | null
#   %entity adv-messages             NAME -> text | null
#   %entity adv-hints                name -> {number,turns,penalty,question,hint}
#   %entity adv-tables               classes/turn_thresholds/obituaries/dwarflocs
#
# Determinism law: this output is a FIXED POINT of madc's own save
# (world_doc_extract/emit): entities ride in creation order, props sort
# alphabetically (std::map), JSON values dump compact with sorted keys
# (nlohmann's ordered dump). Strings follow wt_value_text's ambiguity rule
# exactly — bare unless the bare spelling would re-parse as another kind.
#
# YAML text carries C-escape passthroughs (the reference generator writes
# text into C string literals, so a literal backslash-t in YAML becomes a
# TAB in the compiled binary): we apply the same unescape here.

import json
import sys

import yaml


def c_unescape(s):
    # The reference pipeline's semantics: YAML text -> C string literal ->
    # compiled bytes. Only the escapes that actually appear in the data.
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "n":
                out.append("\n")
                i += 2
                continue
            if n == "t":
                out.append("\t")
                i += 2
                continue
            if n == "\\":
                out.append("\\")
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def jdump(v):
    # nlohmann dump() twin: compact separators, key-sorted objects,
    # raw UTF-8 (no \uXXXX for non-ASCII).
    return json.dumps(v, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)


def is_numeric_spelling(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def prop_text(v):
    # wt_value_text's law, ported: the emitted spelling must re-parse as
    # the same kind and content.
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float) or isinstance(v, (list, dict)):
        return jdump(v)
    s = str(v)
    ambiguous = (
        s == "" and False) or (s != "" and (
        s[0] in "[{\""
        or s in ("true", "false", "null")
        or "\n" in s or "\r" in s
        or s[0] in " \t" or s[-1] in " \t"
        or is_numeric_spelling(s)))
    if ambiguous:
        return jdump(s)
    return s


def clean_text(v):
    return c_unescape(v) if isinstance(v, str) else v


def key_name(k):
    # YAML 1.1 booleanizes bare keys: the action named NO arrives as
    # False (and a hypothetical YES as True). Recover the spelling.
    if k is False:
        return "NO"
    if k is True:
        return "YES"
    return str(k)


class WorldOut:
    def __init__(self):
        self.lines = ["%world 1"]
        self.links = []

    def entity(self, name, props):
        self.lines.append("%entity " + name)
        for k in sorted(props.keys()):
            v = props[k]
            self.lines.append("  " + k + " = " + prop_text(v))

    def link(self, frm, rel, to):
        self.links.append("%link " + frm + " " + rel + " " + to)

    def text(self):
        return "\n".join(self.lines + self.links) + "\n"


def convert(doc):
    out = WorldOut()
    counts = {}

    # Travel-rule verbs in the YAML are vocabulary WORDS (BUILD, ROAD),
    # not motion names — the reference's make_dungeon.py resolves them
    # against the motion word lists. Resolve here too, so the world file
    # carries ONE id space (motion names) and the game's vocabulary
    # lookup and rule matching meet in it. Unresolvable verbs are a data
    # breach: fail loud.
    word_to_motion = {}
    for mname, m in doc["motions"]:
        mname = key_name(mname)
        for w in (m or {}).get("words") or []:
            word_to_motion.setdefault(w.lower(), mname)

    def motion_of(verb_word):
        m = word_to_motion.get(str(verb_word).lower())
        if m is None:
            raise SystemExit("advent_yaml_to_world: travel verb %r "
                             "matches no motion word" % verb_word)
        return m

    # ---- locations -------------------------------------------------------
    locations = doc["locations"]
    counts["locations"] = len(locations)
    travel_rules = 0
    for name, loc in locations:
        name = key_name(name)
        props = {}
        desc = loc.get("description") or {}
        if desc.get("long") is not None:
            props["long"] = clean_text(desc["long"])
        if desc.get("short") is not None:
            props["short"] = clean_text(desc["short"])
        if desc.get("maptag") is not None:
            props["maptag"] = desc["maptag"]
        conds = loc.get("conditions") or {}
        flags = {k: True for k, v in conds.items() if v}
        if flags:
            props["conditions"] = flags
        if loc.get("sound"):
            props["sound"] = loc["sound"]
        if loc.get("loud"):
            props["loud"] = True
        hints = loc.get("hints") or []
        if hints:
            props["hints"] = [h["name"] for h in hints]
        travel = []
        for rule in loc.get("travel") or []:
            r = {"verbs": [motion_of(v) for v in rule.get("verbs") or []],
                 "cond": rule.get("cond"),
                 "action": rule.get("action")}
            if rule.get("nodwarves"):
                r["nodwarves"] = True
            travel.append(r)
            travel_rules += 1
        props["travel"] = travel
        out.entity(name, props)
    counts["travel"] = travel_rules

    # ---- objects ---------------------------------------------------------
    objects = doc["objects"]
    counts["objects"] = len(objects)

    # LISTING-ORDER LAW (plan, A5/A6): the reference's atloc lists PREPEND
    # on drop, and initialise() drops two-placed objects first (objnum
    # DESCENDING; fixed side before primary side each), then plain-placed
    # objects (objnum DESCENDING; fixd <= 0, so immovable one-placed
    # objects ride this loop). Replay that exact order into per-object
    # seq / seq_fixed props; the game lists location contents by seq
    # DESCENDING and stamps every later drop from a world counter seeded
    # with seq_next. YAML list position IS the objnum (index 0 = NO_OBJECT).
    seq_of = {}
    seq_fixed_of = {}
    seq = 0
    numbered = [(i, key_name(n), o or {}) for i, (n, o) in enumerate(objects)]
    for i, name, obj in reversed(numbered):
        locs = obj.get("locations")
        if i >= 1 and isinstance(locs, list) and len(locs) > 1:
            seq += 1
            seq_fixed_of[name] = seq
            seq += 1
            seq_of[name] = seq
    for i, name, obj in reversed(numbered):
        locs = obj.get("locations")
        place = locs[0] if isinstance(locs, list) else locs
        if (i >= 1 and name not in seq_of
                and place and place != "LOC_NOWHERE"):
            seq += 1
            seq_of[name] = seq
    seq_next = seq + 1

    objwords = {}
    for name, obj in objects:
        name = key_name(name)
        obj = obj or {}
        props = {}
        words = obj.get("words") or []
        if words:
            props["words"] = words
            for w in words:
                objwords.setdefault(w, []).append(name)
        if obj.get("inventory") is not None:
            props["inventory"] = clean_text(obj["inventory"])
        for arr in ("states", "descriptions", "changes", "sounds", "texts"):
            if obj.get(arr) is not None:
                props[arr] = [clean_text(x) if x is not None else None
                              for x in obj[arr]]
        if obj.get("immovable"):
            props["immovable"] = True
        if obj.get("treasure"):
            props["treasure"] = True
        if name in seq_of:
            props["seq"] = seq_of[name]
        if name in seq_fixed_of:
            props["seq_fixed"] = seq_fixed_of[name]
        locs = obj.get("locations")
        place = None
        if isinstance(locs, list):
            place = locs[0]
            if len(locs) > 1:
                props["fixed_at"] = locs[1]
        elif isinstance(locs, str):
            place = locs
        out.entity(name, props)
        if place and place != "LOC_NOWHERE":
            out.link(name, "in", place)

    # ---- vocabulary ------------------------------------------------------
    motions = doc["motions"]
    actions = doc["actions"]
    counts["motions"] = len(motions)
    counts["actions"] = len(actions)
    vocab_m = {}
    for name, m in motions:
        name = key_name(name)
        for w in (m or {}).get("words") or []:
            vocab_m.setdefault(w, []).append(name)
    vocab_a = {}
    defaults = {}
    noaction = []
    for name, a in actions:
        name = key_name(name)
        a = a or {}
        for w in a.get("words") or []:
            vocab_a.setdefault(w, []).append(name)
        msg = a.get("message")
        if msg == "NO_MESSAGE":
            msg = None
        defaults[name] = clean_text(msg) if msg is not None else None
        if a.get("noaction"):
            noaction.append(name)
    out.entity("adv-vocabulary", {"motions": vocab_m, "actions": vocab_a,
                                  "objwords": objwords})
    out.entity("adv-action-defaults", defaults)

    # ---- messages --------------------------------------------------------
    messages = doc["arbitrary_messages"]
    counts["messages"] = len(messages)
    msg_props = {}
    for name, text in messages:
        name = key_name(name)
        msg_props[name] = clean_text(text) if text is not None else None
    out.entity("adv-messages", msg_props)

    # ---- hints -----------------------------------------------------------
    hint_props = {}
    for item in doc["hints"]:
        h = item["hint"]
        hint_props[h["name"]] = {
            "number": h["number"], "turns": h["turns"],
            "penalty": h["penalty"],
            "question": clean_text(h["question"]),
            "hint": clean_text(h["hint"])}
    counts["hints"] = len(hint_props)
    out.entity("adv-hints", hint_props)

    # ---- tables ----------------------------------------------------------
    out.entity("adv-tables", {
        "classes": [{"threshold": c["threshold"],
                     "message": clean_text(c["message"])
                     if c["message"] is not None else None}
                    for c in doc["classes"]],
        "turn_thresholds": [{"threshold": t["threshold"],
                             "point_loss": t["point_loss"],
                             "message": clean_text(t["message"])}
                            for t in doc["turn_thresholds"]],
        "obituaries": [{"query": clean_text(o["query"]),
                        "yes_response": clean_text(o["yes_response"])}
                       for o in doc["obituaries"]],
        "dwarflocs": doc["dwarflocs"],
        "noaction": noaction,
        "seq_next": seq_next})

    return out, counts


def check(counts):
    # The A4 spot gate: the shape table the recon measured. A drift here
    # means the vendored YAML changed — re-verify, never re-pin blindly.
    expect = {"locations": 185, "objects": 70, "motions": 76, "actions": 58,
              "travel": 623, "hints": 10}
    ok = True
    for k, want in expect.items():
        got = counts.get(k)
        status = "ok" if got == want else "MISMATCH"
        if got != want:
            ok = False
        print("%-10s %5s (expect %s) %s" % (k, got, want, status))
    print("messages   %5s" % counts.get("messages"))
    return ok


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(
            "usage: advent_yaml_to_world.py adventure.yaml adventure.world\n")
        return 2
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    out, counts = convert(doc)
    if not check(counts):
        sys.stderr.write("advent_yaml_to_world: shape check FAILED\n")
        return 1
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(out.text())
    print("wrote %s" % sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
