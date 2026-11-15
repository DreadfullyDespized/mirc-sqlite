#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys

BODY_MAX_LINES = 12
SEATS = frozenset({
    "lyska",
    "hester quill",
    "hester",
    "sidony quire",
    "sidony",
    "calla wrenfield",
    "calla",
    "ines varro",
    "ines",
})
SEAT_FIX = (
    "Lyska, Hester Quill, Sidony Quire, Calla Wrenfield, Ines Varro "
    "(a first name alone is fine)"
)
HTML_COMMENT_RX = re.compile(r"<!--.*?(?:-->|\Z)", re.S)
FOR_DREAD_RX = re.compile(r"^For Dread:\s*\S")
DOER_RX = re.compile(r"^\s*doer:\s*(.*?)\s*$", re.I)
GRADER_RX = re.compile(r"^\s*grader:\s*(.*?)\s*$", re.I)
TESTED_RX = re.compile(r"^\s*tested:\s*(.*?)\s*$", re.I)
PAREN_RX = re.compile(r"\s*\([^)]*\)\s*$")
SAME_PR = (
    "Fix it on this same PR: edit the PR body (it re-runs on edit). "
    "Do not close and reopen a copy. Do not rerun CI without a change."
)


def strip_body(body):
    text = (body or "").replace("\r", "")
    return HTML_COMMENT_RX.sub("", text)


def nonblank_lines(text):
    return [ln for ln in text.split("\n") if ln.strip()]


def seat_value(raw):
    return PAREN_RX.sub("", raw).strip()


def check_for_dread(lines):
    if lines and FOR_DREAD_RX.match(lines[0]):
        return []
    return [
        "pr_body_check: the first non-blank line does not match ^For Dread:\\s*\\S. "
        "Fix: start the body with 'For Dread: <what this PR is>'."
    ]


def check_length(lines):
    if len(lines) <= BODY_MAX_LINES:
        return []
    return [
        "pr_body_check: the body has %d non-blank lines, which is more than %d. "
        "Fix: delete non-blank lines until %d or fewer remain."
        % (len(lines), BODY_MAX_LINES, BODY_MAX_LINES)
    ]


def check_roles(lines):
    doers = []
    graders = []
    for ln in lines:
        doer = DOER_RX.match(ln)
        if doer:
            doers.append(seat_value(doer.group(1)))
        grader = GRADER_RX.match(ln)
        if grader:
            graders.append(seat_value(grader.group(1)))
    out = []
    if len(doers) != 1:
        out.append(
            "pr_body_check: found %d Doer: lines, expected exactly one. "
            "Fix: leave one line 'Doer: <current seat>'." % len(doers)
        )
    if len(graders) != 1:
        out.append(
            "pr_body_check: found %d Grader: lines, expected exactly one. "
            "Fix: leave one line 'Grader: <current seat or self-merge>'." % len(graders)
        )
    if len(doers) == 1 and doers[0].casefold() not in SEATS:
        out.append(
            "pr_body_check: Doer value %r is not a current seat. "
            "Fix: set Doer to one of %s." % (doers[0], SEAT_FIX)
        )
    if len(graders) == 1 and graders[0].casefold() != "self-merge" and graders[0].casefold() not in SEATS:
        out.append(
            "pr_body_check: Grader value %r is not a current seat. "
            "Fix: set Grader to one of %s, or exactly self-merge." % (graders[0], SEAT_FIX)
        )
    if len(doers) == 1 and len(graders) == 1 and doers[0].casefold() == graders[0].casefold():
        out.append(
            "pr_body_check: Doer and Grader are the same. "
            "Fix: set Grader to a different current seat, or exactly self-merge."
        )
    return out


def check_tested(lines):
    for ln in lines:
        tested = TESTED_RX.match(ln)
        if tested and tested.group(1).strip():
            return []
    return [
        "pr_body_check: there is no Tested: line with non-empty text. "
        "Fix: add a line 'Tested: <command you ran and the result>'."
    ]


def problems(body):
    lines = nonblank_lines(strip_body(body))
    out = []
    out.extend(check_for_dread(lines))
    out.extend(check_length(lines))
    out.extend(check_roles(lines))
    out.extend(check_tested(lines))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Check a PR body for the repo-loop shape.")
    ap.add_argument("--event", help="GitHub event JSON ($GITHUB_EVENT_PATH)")
    ap.add_argument("--body-file")
    a = ap.parse_args(argv)
    if a.event:
        with open(a.event, encoding="utf-8") as fh:
            event = json.load(fh)
        pr = event.get("pull_request")
        if not pr:
            print("pr_body_check: not a pull_request event; nothing to check")
            return 0
        body = pr.get("body") or ""
    elif a.body_file:
        with open(a.body_file, encoding="utf-8") as fh:
            body = fh.read()
    else:
        ap.error("pass --event or --body-file")
    found = problems(body)
    for line in found:
        print(line)
    if found:
        print(SAME_PR)
        return 1
    print("pr_body_check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
