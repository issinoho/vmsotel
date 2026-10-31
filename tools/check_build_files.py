#!/usr/bin/env python3
"""Checks that the three build files compile the same agent sources.

A source added to the Makefile but not to build_vms.com or descrip.mms
builds here and fails quietly on a machine nobody here can see (CLAUDE.md,
"A new source file touches three build files"). This makes that a test.

    check_build_files.py Makefile build_vms.com descrip.mms
"""

import re
import sys


def vms_to_posix(spec):
    """[.src.model]model.c -> src/model/model.c"""
    m = re.fullmatch(r"\[\.([\w.]+)\](\w+\.c)", spec, re.I)
    return m.group(1).replace(".", "/") + "/" + m.group(2) if m else None


def main(makefile, com, mms):
    mk = open(makefile).read()
    # Agent sources: everything under src/ that the Makefile compiles.
    make_srcs = set(re.findall(r"^\$\(B\)/\w+\.o: (src/\S+\.c)", mk, re.M))

    com_text = open(com).read()
    com_srcs = {vms_to_posix(s) for s in
                re.findall(r'csrc = "([^"]+)"', com_text)}

    mms_text = open(mms).read()
    mms_srcs = {vms_to_posix(s) for s in
                re.findall(r"^\[\.build\]\w+\.obj : (\[[^\]]+\]\w+\.c)",
                           mms_text, re.M)}

    failures = 0
    for name, srcs in (("build_vms.com", com_srcs), ("descrip.mms", mms_srcs)):
        # The fake source is Linux-only; the VMS files build the VMS one.
        want = {s for s in make_srcs if "/fake/" not in s}
        missing, extra = want - srcs, srcs - want
        if missing or extra:
            failures += 1
            print(f"FAIL  {name}: missing {sorted(missing)}, "
                  f"not in Makefile {sorted(extra)}")
        else:
            print(f"ok    {name} compiles the Makefile's {len(want)} "
                  f"agent sources")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
