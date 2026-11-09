#!/usr/bin/env python3
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
"""Writes a readable script of an NPC's engine-side topics (data/dialog/NAME.json) as Markdown.

    python3 -I tools/dialog_doc.py data/dialog/gwillim.json docs/gwillim_dialogue_script.md [INTRO.md]

The JSON stays the source: edit it, then run this again. INTRO.md (optional) is put first.
"""
import json
import sys


def cond_text(c, stages):
    if not c:
        return "always"
    parts = []
    if c.get("stage"):
        parts.append("stage " + " / ".join(c["stage"] if isinstance(c["stage"], list) else [c["stage"]]))
    if c.get("from_stage"):
        parts.append("from stage " + c["from_stage"])
    if c.get("before_stage"):
        parts.append("before stage " + c["before_stage"])
    for k, word in (("step_done", "after"), ("step_not_done", "until")):
        v = c.get(k)
        if v:
            v = v if isinstance(v, list) else [v]
            parts.append(word + " " + ", ".join("`" + s + "`" for s in v))
    for k, word in (("asked", "asked"), ("not_asked", "not yet asked")):
        v = c.get(k)
        if v:
            parts.append(word + " " + ", ".join(v if isinstance(v, list) else [v]))
    if c.get("avatar_int"):
        parts.append("the Avatar's INT at least %d" % c["avatar_int"])
    if "cond" in c:
        parts.append("condition `" + json.dumps(c["cond"]) + "`")
    return "; ".join(parts) or "always"


def lines(out, say, indent):
    for l in say:
        out.append(indent + "> " + l)


def topic(out, t, depth, stages):
    ind = "  " * depth
    head = "**" + t.get("label", t.get("id")) + "**" if depth == 0 else "*Avatar:* \"" + t.get("label", t.get("id")) + "\""
    when = t.get("when")
    out.append(ind + "- " + head + (" (" + cond_text(when, stages) + ")" if when else ""))
    lines(out, t.get("say", []), ind + "  ")
    for a in t.get("alt", []):
        out.append(ind + "  - *instead, " + cond_text(a.get("when"), stages) + ":*")
        lines(out, a.get("say", []), ind + "    ")
    for s in t.get("topics", []):
        topic(out, s, depth + 1, stages)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = json.load(open(src))
    out = []
    if len(sys.argv) > 3:
        out.append(open(sys.argv[3]).read().rstrip() + "\n")
    m = d.get("menu", {})
    out.append("## Stages\n")
    out.append("The current stage is the last one whose condition holds (critical-path step ids from "
               "`engine/data/critpath.json`; a step also counts as done when a later milestone is).\n")
    out.append("| Stage | Meaning | When |")
    out.append("| --- | --- | --- |")
    for s in d.get("stages", []):
        out.append("| `%s` | %s | %s |" % (s["id"], s.get("title", ""), cond_text(s.get("when"), None)))
    out.append("")
    out.append("## Topics (in story order)\n")
    out.append("Each top-level topic is an answer in %s's own menu. Topics in the same slot replace each other "
               "(the last one below whose condition holds is shown); at most %d slot(s) at a time. Inside a topic "
               "\"%s\" goes back to his own menu (\"%s\" from a deeper menu).\n"
               % (d.get("name"), m.get("max_entries", 1), m.get("leave", "Something else."), m.get("back", "Back.")))
    slot = None
    for t in d.get("topics", []):
        s = t.get("slot") or t.get("id")
        if s != slot:
            out.append("\n### Slot: %s\n" % s)
            slot = s
        topic(out, t, 0, None)
    if d.get("scenes"):
        out.append("\n## Scene patches (fixes to original scenes)\n")
        for sc in d["scenes"]:
            out.append("### %s (usecode class %s; %s)\n" % (sc["id"], sc["class"], cond_text(sc.get("when"), None)))
            if sc.get("about"):
                out.append(sc["about"] + "\n")
            out.append("Said first, before the script's own line:\n")
            lines(out, sc.get("say", []), "")
            out.append("")
            for t in sc.get("topics", []):
                topic(out, t, 1, None)
            out.append("\nThen the original line follows and the scene goes on as before.\n")
    open(dst, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
