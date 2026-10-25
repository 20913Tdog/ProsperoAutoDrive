#!/usr/bin/env python3
# Prospero AutoDrive - CI guard: no personal data in tracked files.
#
# Copyright (C) 2026 Prospero AutoDrive contributors
# SPDX-License-Identifier: MIT
#
# Fails on what could point to a person or a machine:
#   - public IPv4 addresses (private, loopback, link-local and the
#     documentation ranges 192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24 are
#     fine; specification section numbers such as "MS-SMB2 3.3.1.2" and
#     version="6.0.0.0" are not addresses)
#   - e-mail addresses other than the GitHub noreply ones
#   - user profile paths with a real name (C:\Users\<name>, /home/<name>)
# Usage: check-sanitized.py  (in the repository root)

import ipaddress
import re
import subprocess
import sys

IPV4 = re.compile(r"(?<![\w.])(\d{1,3}(?:\.\d{1,3}){3})(?![\w.])")
NOT_AN_ADDRESS = re.compile(r"(MS-[A-Z0-9]+\s+|version=\"|[Vv]ersion\s+|section\s+)$")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
EMAIL_OK = re.compile(r"(users\.noreply\.github\.com|noreply@anthropic\.com)$")
PROFILE = re.compile(r"(?i)(?:[A-Z]:[\\/]+Users[\\/]+|/home/)([A-Za-z0-9._-]+)")
PROFILE_OK = {"runner", "public", "default", "name", "user", "username", "du", "you"}
DOC_NETS = [ipaddress.ip_network(n) for n in ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24")]


def public(text):
    try:
        a = ipaddress.ip_address(text)
    except ValueError:
        return False
    if a.is_private or a.is_loopback or a.is_link_local or a.is_multicast or a.is_unspecified:
        return False
    if a.is_reserved or any(a in n for n in DOC_NETS) or text == "255.255.255.255":
        return False
    return True


def main():
    files = subprocess.run(["git", "ls-files", "-z"], capture_output=True, check=True).stdout.split(b"\0")
    problems = []
    for name in (f.decode() for f in files if f):
        try:
            with open(name, encoding="utf-8") as fh:
                lines = fh.read().splitlines()
        except (UnicodeDecodeError, OSError):
            continue          # binaries are refused by another guard, submodules are folders
        if name == ".github/scripts/check-sanitized.py":
            continue
        for no, line in enumerate(lines, 1):
            for m in IPV4.finditer(line):
                if public(m.group(1)) and not NOT_AN_ADDRESS.search(line[: m.start()]):
                    problems.append(f"{name}:{no}: public IPv4 address {m.group(1)}")
            for m in EMAIL.finditer(line):
                if not EMAIL_OK.search(m.group(0)):
                    problems.append(f"{name}:{no}: e-mail address")
            for m in PROFILE.finditer(line):
                if m.group(1).lower() not in PROFILE_OK and not m.group(1).startswith(("%", "<", "$")):
                    problems.append(f"{name}:{no}: user profile path ({m.group(0)})")
    for p in problems:
        print(f"::error::{p}")
    print(f"checked {len(files) - 1} files, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
