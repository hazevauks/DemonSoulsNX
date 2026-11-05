#!/usr/bin/env python3
"""Fails if the repository holds game data, code made from the game, keys, or personal
information.

Checks what git tracks or is about to commit. Run it before a commit; CI runs it on every push.
"""

import pathlib
import re
import subprocess
import sys

# the game's files, the PS3's executables and firmware, and builds made from them
FORBIDDEN_NAME = re.compile(
    r"\.(iso|pup|sfb|sfo|trp|bin|elf|self|prx|sprx|sdat|unedat|edat|rap|rif|pam|dcx|tpf|flver|hkx|msb|nvm"
    r"|mowb|mosb|moib|sib|fsb|fev|nro|nso|nacp|npdm|dksh)$"
    r"|bnd$|bnd\.dcx$"
    r"|(^|/)(game_data|work|generated)/",
    re.IGNORECASE,
)

# personal information, and key tables
FORBIDDEN_TEXT = [
    (re.compile(rb"[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s\"'<>*]+"), "a path inside a home folder"),
    (re.compile(rb"/home/(?!runner/)[a-z][\w.-]+/|/c/Users/[^/\s]+"), "a path inside a home folder"),
    (re.compile(rb"[\w.+-]+@(gmail|hotmail|outlook|live|yahoo|proton(mail)?|icloud|uol|bol|terra)\.[a-z.]+", re.IGNORECASE),
     "a personal mail address"),
    (re.compile(rb"\b(SDAT|EDAT|PKG|GPKG|KLIC|NP_OMAC|NP_PSX|NP_PSP|NP_KLIC|RIF|ERK|RIV|SELF|APPLDR|ISOLDR|LV[0-2])_?\w*KEY\w*"
                rb"\s*(\[\s*\d*\s*\])?\s*=\s*(\{|b[\"']|bytes|bytearray)", re.IGNORECASE),
     "a key table"),
]

TEXT_SUFFIXES = {
    ".md", ".txt", ".py", ".sh", ".ps1", ".bat", ".yml", ".yaml", ".json", ".toml", ".cmake", ".cfg", ".ini",
    ".c", ".h", ".cc", ".cpp", ".hpp", ".inl", ".m", ".mm", ".glsl", ".vert", ".frag", ".hlsl",
}
TEXT_NAMES = {"CMakeLists.txt", ".gitignore", ".gitattributes"}
SIZE_LIMIT = 8 * 1024 * 1024


def git_files():
    output = subprocess.run(["git", "ls-files", "--cached", "-z"], capture_output=True, check=True).stdout
    return [name.decode("utf-8", "replace") for name in output.split(b"\0") if name]


def main():
    files = git_files()
    problems = []
    for name in files:
        if FORBIDDEN_NAME.search(name):
            problems.append("%s: not allowed in the repository" % name)
            continue
        path = pathlib.Path(name)
        if not path.is_file():
            continue
        if path.stat().st_size > SIZE_LIMIT:
            problems.append("%s: larger than 8 MB" % name)
            continue
        if path.name == "check_repo.py" or not (path.suffix.lower() in TEXT_SUFFIXES or path.name in TEXT_NAMES):
            continue
        data = path.read_bytes()
        for pattern, what in FORBIDDEN_TEXT:
            # third-party authors' addresses are in their license notices, which must stay
            if what == "a personal mail address" and name.startswith("third_party/"):
                continue
            match = pattern.search(data)
            if match:
                problems.append("%s: contains %s (%s)" % (name, what, match.group(0)[:40].decode("latin-1")))

    for problem in problems:
        print("ERROR: " + problem)
    print("%d files checked, %d problems" % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
