#!/usr/bin/env python3
"""Builds skillz.md = tools/skillz_body.md + generated appendices (ReAPI hooks, Ham functions). Run after tools/gen_reapi.py and tools/gen_ham.py."""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
body = open(os.path.join(root, "tools/skillz_body.md"), encoding="utf-8").read().rstrip() + "\n"
def section(path, title):
    t = open(os.path.join(root, path), encoding="utf-8").read().split("\n")
    return "\n## " + title + "\n" + "\n".join(t[1:]).strip() + "\n"
out = body + section("docs/REAPI_HOOKS.md", "Appendix A — ReAPI hookchains (event `reapi:<Hook>[:post]`)") + section("docs/HAM_FUNCTIONS.md", "Appendix B — Ham functions (event `ham:<func>:<classname>[:post]`)")
open(os.path.join(root, "skillz.md"), "w", encoding="utf-8").write(out)
print("skillz.md:", len(out), "bytes")
