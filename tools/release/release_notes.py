#!/usr/bin/env python3
"""Writes the description of a pre-release.

The description is, in this order:

1. The state of the project (.github/release-state.md): whether the game is
   playable, and what does not work yet.
2. The big changes and additions since the last pre-release, read from the
   commits between that pre-release's tag and the commit being released:
   - each `Release-Note:` trailer of a commit is one entry;
   - a commit without one is listed by its subject;
   - a commit with `Release-Note: none` is left out (small fixes, tidying).
3. How to download and start it (.github/prerelease-notes.md).

Only the standard library and git are used.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

TRAILER = "Release-Note"
TAG_PATTERN = re.compile(r"^v\d+(?:\.\d+)*-pre\.(\d+)$")


def git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout


class Previous(NamedTuple):
    tag: str
    commit: str  # In `head`'s history: where the changes are counted from.


def previous_release(repo: Path, head: str, tag: str) -> Previous | None:
    """The last pre-release before `tag`: the one with the highest run
    number below `tag`'s (or, if `tag` has none, the highest).

    Its tag normally is in `head`'s history. If the history was rewritten
    since (the commits are new, the files the same), the commit of `head`'s
    history with the same files as the tag stands for it."""
    own = TAG_PATTERN.match(tag)
    limit = int(own.group(1)) if own else None
    best: tuple[int, str] | None = None
    for name in git(repo, "tag", "--list", "v*").split():
        match = TAG_PATTERN.match(name)
        if not match or name == tag:
            continue
        number = int(match.group(1))
        if limit is not None and number >= limit:
            continue
        if best is None or number > best[0]:
            best = (number, name)
    if best is None:
        return None
    name = best[1]
    commit = git(repo, "rev-parse", f"{name}^{{commit}}").strip()
    contained = subprocess.run(
        ["git", "-C", str(repo), "merge-base", "--is-ancestor", commit, head],
        capture_output=True,
    ).returncode == 0
    if contained:
        return Previous(name, commit)
    tree = git(repo, "rev-parse", f"{name}^{{tree}}").strip()
    for line in git(repo, "log", "--format=%H %T", head).splitlines():
        sha, files = line.split()
        if files == tree:
            return Previous(name, sha)
    return None


def changes(repo: Path, since: str | None, head: str) -> list[str]:
    """One line for each change, newest first, without duplicates."""
    span = f"{since}..{head}" if since else head
    # Fields are split by the unit separator and commits by the record
    # separator, which no commit message holds.
    log = git(
        repo,
        "log",
        "--no-merges",
        f"--format=%s%x1f%(trailers:key={TRAILER},valueonly,separator=%x1f)%x1e",
        span,
    )
    entries: list[str] = []
    for record in log.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        subject, *notes = record.split("\x1f")
        notes = [n.strip() for n in notes if n.strip()]
        if not notes:
            notes = [subject.strip()]
        for note in notes:
            if note.lower() == "none" or note in entries:
                continue
            entries.append(note)
    return entries


def compose(state: str, body: str, tag: str, since: Previous | None,
            entries: list[str], repository: str | None) -> str:
    parts = [state.strip()]
    if since:
        lines = [f"## Changes since {since.tag}", ""]
        lines += [f"- {e}" for e in entries] or ["- Nothing that changes what the program does."]
        if repository:
            lines += ["", f"Every commit: https://github.com/{repository}/compare/{since.commit}...{tag}"]
    else:
        lines = ["## Changes", "", "- The first pre-release."]
    parts.append("\n".join(lines))
    parts.append(body.strip())
    return "\n\n".join(parts) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True, help="the tag of the pre-release being made")
    parser.add_argument("--head", default="HEAD", help="the commit being released")
    parser.add_argument("--repo", type=Path, default=Path("."), help="the git checkout")
    parser.add_argument("--repository", help="owner/name on GitHub, for the link to every commit")
    parser.add_argument("--state", type=Path, default=Path(".github/release-state.md"))
    parser.add_argument("--body", type=Path, default=Path(".github/prerelease-notes.md"))
    parser.add_argument("--out", type=Path, help="where to write it (default: standard output)")
    args = parser.parse_args(argv)

    since = previous_release(args.repo, args.head, args.tag)
    text = compose(
        args.state.read_text(encoding="utf-8"),
        args.body.read_text(encoding="utf-8"),
        args.tag,
        since,
        changes(args.repo, since.commit if since else None, args.head),
        args.repository,
    )
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
