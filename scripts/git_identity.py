# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared Git identity contract for repository tooling.

The Python half of ``scripts/git-identity.sh``. Tooling that replays or
audits a checkout creates commits, but it never carries an identity of its
own: it commits as whoever runs it. Call :func:`require_git_identity` before
any such step.

``user.useConfigOnly`` stops Git from inventing an identity from the account
and host names, so the check succeeds only when the environment or the user's
configuration supplies one. The resolved values are exported into the process
environment for helpers that run Git with a suppressed global configuration.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True

ADVICE = (
    "Git identity is not configured: set user.name and user.email "
    "(git config --global) or GIT_COMMITTER_NAME/GIT_COMMITTER_EMAIL"
)

_IDENT = re.compile(r"^(?P<name>.+) <(?P<email>[^>]+)> [^ ]+ [^ ]+$")


def _resolve(variable: str) -> tuple[str, str] | None:
    result = subprocess.run(
        ["git", "-c", "user.useConfigOnly=true", "var", variable],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    match = _IDENT.match(result.stdout.decode(errors="replace").strip())
    if match is None:
        return None
    return match.group("name"), match.group("email")


def require_git_identity(context: str) -> tuple[str, str]:
    """Return the committer name and email, or exit with the shared advice."""
    committer = _resolve("GIT_COMMITTER_IDENT")
    if committer is None:
        raise SystemExit(f"{context}: {ADVICE}")
    # Replayed commits keep the author recorded in their mail patch. An author
    # is still needed for a commit created from scratch, and a caller who named
    # only a committer has named who is running the tool.
    author = _resolve("GIT_AUTHOR_IDENT") or committer
    os.environ["GIT_AUTHOR_NAME"], os.environ["GIT_AUTHOR_EMAIL"] = author
    os.environ["GIT_COMMITTER_NAME"], os.environ["GIT_COMMITTER_EMAIL"] = committer
    return committer
