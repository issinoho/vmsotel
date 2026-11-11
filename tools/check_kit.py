#!/usr/bin/env python3
"""Checks the PCSI kit's pieces against each other, since PRODUCT
PACKAGE only runs on VMS and fails there one file at a time.

  - every file the product description names is gathered into the
    material directory by kit/make_kit.com, and every file gathered is
    named (a stray one is harmless to PCSI but means the two drifted);
  - every file it gathers comes from a file in this repository, or is
    one of the images build_vms makes;
  - every file's directory is declared, and the text module's product
    line is the description's;
  - the templates use only the tokens make_kit fills in;
  - the procedures the kit layout relies on are in the kit.

    check_kit.py            (from the repository root)
"""

import os
import re
import sys

IMAGES = {"VMSOTEL.EXE", "VMSOTEL_TLS.EXE", "VMSOTEL_LAUNCH.EXE"}
TOKENS = {"PRODUCER", "BASE", "PCSIVERSION", "VERSION", "ARCH", "NOTVAX"}


def repo_path(vms_dir, name):
    """[] / [.KIT] / [.DOCS] and a VMS name -> the repository's file,
    whose name is the same in lower case (VMSOTEL.CONF.EXAMPLE is
    vmsotel.conf.example; LICENSE. is LICENSE)."""
    d = vms_dir.strip("[]").lstrip(".").replace(".", "/").lower()
    n = name.rstrip(".") if name.endswith(".") else name.lower()
    return os.path.join(d, n) if d else n


def gathered(com):
    """{name in the material directory: source} from make_kit's copies
    and fills."""
    out = {}
    for line in com.split("\n"):
        # Some only off VAX: "$ if arch .nes. "VAX" then copy/nolog ...".
        # The VAX-only copies are of the same files under ODS-2 names
        # (VMSOTEL.CONF_EXAMPLE), which the repository does not have.
        if line.startswith('$ if arch .eqs. "VAX" then copy/nolog'):
            continue
        m = re.match(r"\$ (?:if .* then )?copy/nolog (\S+) 'mat'(\S*)$",
                     line)
        if m:
            src, rename = m.group(1), m.group(2)
            d = re.match(r"(\[[^\]]*\])", src).group(1)
            names = src[len(d):].split(",")
            for n in names:
                dest = rename if rename else n
                out[dest.upper()] = (d, n)
    # README.VMS is filled from a template, not copied.
    m = re.search(r'\$ in = "(\[[^\]]*\])(\S+)"\n\$ to = mat \+ "(\S+)"', com)
    if m:
        out[m.group(3).upper()] = (m.group(1), m.group(2))
    return out


def main():
    fails = []
    pdf = open("kit/vmsotel.pcsi$desc").read()
    ptf = open("kit/vmsotel.pcsi$text").read()
    com = open("kit/make_kit.com").read()

    # "@NOTVAX@file ...": a file of every kit but VAX's.
    files = re.findall(r"^\s*(?:@NOTVAX@)?file \[([^\]]+)\](\S+) ;", pdf,
                       re.M)
    dirs = set(re.findall(r"^\s*directory \[([^\]]+)\] ;", pdf, re.M))
    named = {n.upper() for _, n in files}
    have = gathered(com)

    for d, n in files:
        if d != "SYS$STARTUP" and d not in dirs:
            fails.append(f"[{d}]{n}: its directory is not declared")
    for n in sorted(named - set(have)):
        fails.append(f"{n}: named in the product description, not gathered "
                     "by make_kit.com")
    for n in sorted(set(have) - named):
        fails.append(f"{n}: gathered by make_kit.com, not in the product "
                     "description")
    for n, (d, src) in sorted(have.items()):
        if d.upper() == "[.BUILD]":
            if src.upper() not in IMAGES:
                fails.append(f"{n}: from [.BUILD], but build_vms makes no "
                             f"{src}")
            continue
        p = repo_path(d, src)
        if not os.path.isfile(p):
            fails.append(f"{n}: gathered from {d}{src}, but there is no {p}")

    for name, text in (("description", pdf), ("text module", ptf),
                       ("README.VMS", open("kit/readme.vms").read())):
        for t in set(re.findall(r"@([A-Z]+)@", text)) - TOKENS:
            fails.append(f"the {name} uses @{t}@, which make_kit does not "
                         "fill in")
    pdf_product = re.search(r"^product (.*?) full ;", pdf, re.M)
    ptf_product = re.search(r"^=product (.*?) full$", ptf, re.M)
    if not pdf_product or not ptf_product or \
            pdf_product.group(1) != ptf_product.group(1):
        fails.append("the text module's product line is not the "
                     "description's")
    for need in ("VMSOTEL_START.COM", "VMSOTEL_RUN.COM", "VMSOTEL_STOP.COM",
                 "VMSOTEL_PRIVS.COM", "VMSOTEL_LAUNCH.EXE",
                 "VMSOTEL$STARTUP.COM", "VMSOTEL$CONFIGURE.COM"):
        if need not in named:
            fails.append(f"{need}: the kit layout needs it, and the product "
                         "description does not name it")

    for f in fails:
        print("FAIL  kit: " + f)
    if not fails:
        print(f"ok    the kit's pieces agree: {len(files)} files in "
              f"{len(dirs)} directories, each gathered from the "
              "repository or the build")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
