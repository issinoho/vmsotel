#!/usr/bin/env python3
"""Checks that the three build files build the agent the same way.

A source, include directory or define added to the Makefile but not to
build_vms.com or descrip.mms builds here and fails quietly on a machine
nobody here can see (CLAUDE.md, "A new source file touches three build
files"). This makes that a test. It compares:

  - the agent's sources (the VMS files build the VMS source, not the
    fake one)
  - the include directories
  - which objects are built with VO_WITH_TLS, in all three
  - which take _SOCKADDR_LEN and /POINTER_SIZE=32, in the two VMS files

    check_build_files.py Makefile build_vms.com descrip.mms
"""

import re
import sys


def vms_dir(spec):
    """[.src.model] -> src/model"""
    return spec.strip("[]").lstrip(".").replace(".", "/").lower()


def vms_src(spec):
    """[.src.model]model.c -> src/model/model.c"""
    m = re.fullmatch(r"(\[\.[\w.]+\])(\w+\.c)", spec, re.I)
    return vms_dir(m.group(1)) + "/" + m.group(2).lower() if m else None


def continued(text, start, mark):
    """The logical line beginning at `start`, joined over lines that end
    in `mark` (a backslash for make, a hyphen for DCL and MMS)."""
    lines, out = text[start:].split("\n"), []
    for line in lines:
        out.append(line)
        if not line.rstrip().endswith(mark):
            break
    return " ".join(out)


def makefile(path):
    t = open(path).read()
    srcs = set(re.findall(r"^\$\(B\)/\w+\.o: (src/\S+\.c)", t, re.M))
    inc = continued(t, t.index("AGENT_INC ="), "\\")
    dirs = set(re.findall(r"-I(src/[\w/]+)", inc))
    flags = {}
    for obj, recipe in re.findall(r"^\$\(B\)/(\w+)\.o: src/\S+\.c.*\n\t(.*)",
                                  t, re.M):
        flags[obj] = {"tls": "-DVO_WITH_TLS" in recipe}
    return srcs, dirs, flags


def build_vms(path):
    t = open(path).read()
    srcs = {vms_src(s) for s in re.findall(r'csrc = "([^"]+)"', t)}
    base = continued(t, t.index("$ cc_base ="), "-")
    dirs = {vms_dir(d) for d in re.findall(r"\[\.src[\w.]*\]", base, re.I)}
    flags, cobj, cflags = {}, None, ""
    for line in t.split("\n"):
        m = re.match(r'\$ cobj = "(\w+)"', line)
        if m:
            cobj = m.group(1)
        m = re.match(r"\$ cflags = (.*)", line)
        if m:
            cflags = m.group(1)
        if line.strip() == "$ gosub compile":
            flags[cobj] = {"tls": "VO_WITH_TLS" in cflags,
                           "sockaddr": "_SOCKADDR_LEN" in cflags,
                           "ptr32": "POINTER_SIZE=32" in cflags}
    return srcs, dirs, flags


def descrip_mms(path):
    t = open(path).read()
    srcs, flags = set(), {}
    for m in re.finditer(r"^\[\.build\](\w+)\.obj : (\[[^\]]+\]\w+\.c)", t,
                         re.M):
        srcs.add(vms_src(m.group(2)))
        rule = t[m.end():t.find("\n\n", m.end())]
        flags[m.group(1)] = {"tls": "VO_WITH_TLS" in rule,
                             "sockaddr": "_SOCKADDR_LEN" in rule,
                             "ptr32": "POINTER_SIZE=32" in rule}
    inc = continued(t, t.index("INCLUDES ="), "-")
    dirs = {vms_dir(d) for d in re.findall(r"\[\.src[\w.]*\]", inc, re.I)}
    return srcs, dirs, flags


def main(mk, com, mms):
    failures = []

    def same(what, a_name, a, b_name, b):
        if a != b:
            failures.append(f"{what}: {a_name} has {sorted(a - b)} that "
                            f"{b_name} lacks, and lacks {sorted(b - a)}")

    m_srcs, m_dirs, m_flags = makefile(mk)
    c_srcs, c_dirs, c_flags = build_vms(com)
    d_srcs, d_dirs, d_flags = descrip_mms(mms)

    want = {s for s in m_srcs if "/fake/" not in s}
    same("sources", "Makefile", want, "build_vms.com", c_srcs)
    same("sources", "Makefile", want, "descrip.mms", d_srcs)
    same("include directories", "Makefile", m_dirs, "build_vms.com", c_dirs)
    same("include directories", "Makefile", m_dirs, "descrip.mms", d_dirs)

    def with_flag(flags, f):
        return {o for o, fl in flags.items() if fl.get(f)}

    m_tls = with_flag(m_flags, "tls")
    same("objects built with VO_WITH_TLS", "Makefile", m_tls,
         "build_vms.com", with_flag(c_flags, "tls"))
    same("objects built with VO_WITH_TLS", "Makefile", m_tls,
         "descrip.mms", with_flag(d_flags, "tls"))
    for f, what in (("sockaddr", "_SOCKADDR_LEN"), ("ptr32", "/POINTER_SIZE=32")):
        same(f"objects built with {what}", "build_vms.com",
             with_flag(c_flags, f), "descrip.mms", with_flag(d_flags, f))
    same("objects", "build_vms.com", set(c_flags), "descrip.mms", set(d_flags))

    for f in failures:
        print("FAIL  " + f)
    if not failures:
        print(f"ok    the three build files agree: {len(want)} agent sources, "
              f"{len(m_dirs)} include directories, {len(m_tls)} TLS objects, "
              f"_SOCKADDR_LEN on {sorted(with_flag(c_flags, 'sockaddr'))}, "
              f"/POINTER_SIZE=32 on {sorted(with_flag(c_flags, 'ptr32'))}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
