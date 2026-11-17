#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

import pr_body_check

GRADE_FAIL_RX = re.compile(r"^Grade:\s*FAIL\b", re.I | re.M)
SHA_RX = re.compile(r"\b([0-9a-f]{7,40})\b", re.I)
PROOF_ITEM_RX = re.compile(r"\bProof item\b", re.I)
LOG_URL_RX = re.compile(r"https?://\S+", re.I)
SAME_PR = (
    "Fix it on this same PR: push the fix or edit the PR body (a body edit re-runs the check). "
    "Do not close and reopen a copy. Do not rerun CI without a change."
)
UNFIXED_FAIL = (
    "prior_fail_check: the newest Grade: FAIL comment (%s) has no later commit besides main merges. "
    "Fix: push commits that address the FAIL blockers."
)
PROOF_NO_LOG = (
    "prior_fail_check: the PR body names a Proof item and has no linked live log. "
    "Fix: add a Tested: link to the live proof log."
)
GIT_FIX = (
    "prior_fail_check: git could not resolve the PR history (%s). "
    "Fix: run this check in a checkout with fetch-depth 0 that contains the base, head, and graded SHAs."
)
COMMENTS_FIX = (
    "prior_fail_check: could not read the PR comments (%s). "
    "Fix: grant pull-requests: read and pass GITHUB_TOKEN, then re-run."
)


def strip_body(body):
    return pr_body_check.strip_body(body)


def names_proof_item(body):
    return bool(PROOF_ITEM_RX.search(strip_body(body)))


def has_linked_live_log(body):
    return bool(LOG_URL_RX.search(strip_body(body)))


def proof_item_problems(body):
    if names_proof_item(body) and not has_linked_live_log(body):
        return [PROOF_NO_LOG]
    return []


def fail_comments(comments):
    found = []
    for comment in comments or []:
        if GRADE_FAIL_RX.search(comment.get("body") or ""):
            found.append(comment)
    found.sort(key=lambda c: c.get("created_at") or "")
    return found


def newest_fail(comments):
    found = fail_comments(comments)
    if found:
        return found[-1]
    return None


def iso(ts):
    text = (ts or "").strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    return text


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True)


def git_ok(*args):
    proc = git(*args)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip() or "exit %d" % proc.returncode
        raise RuntimeError("%s: %s" % (" ".join(args), detail))
    return proc.stdout


def is_commit(sha):
    sha = (sha or "").strip()
    if not sha:
        return False
    proc = git("rev-parse", "--verify", "%s^{commit}" % sha)
    return proc.returncode == 0


def resolve_commit(sha):
    sha = (sha or "").strip()
    if not sha:
        return ""
    proc = git("rev-parse", "--verify", "%s^{commit}" % sha)
    if proc.returncode != 0:
        return ""
    return proc.stdout.strip()


def is_ancestor(commit, tip):
    proc = git("merge-base", "--is-ancestor", commit, tip)
    return proc.returncode == 0


def sha_from_comment(body):
    candidates = SHA_RX.findall(body or "")
    full = []
    short = []
    for token in candidates:
        resolved = resolve_commit(token)
        if not resolved:
            continue
        if len(token) == 40:
            full.append(resolved)
        else:
            short.append(resolved)
    if full:
        return full[-1]
    if short:
        return short[-1]
    return ""


def commit_parents(sha):
    raw = git_ok("log", "-1", "--format=%P", sha).strip()
    if not raw:
        return []
    return raw.split()


def commit_date(sha):
    return iso(git_ok("log", "-1", "--format=%cI", sha).strip())


def first_parent_range(start, head):
    listed = git_ok("rev-list", "--first-parent", "--reverse", "%s..%s" % (start, head))
    return [line.strip() for line in listed.splitlines() if line.strip()]


def is_main_merge(sha, base):
    parents = commit_parents(sha)
    if len(parents) < 2:
        return False
    return any(is_ancestor(parent, base) for parent in parents)


def commits_after_fail(fail, head, base):
    graded = sha_from_comment(fail.get("body") or "")
    if graded:
        if not is_ancestor(graded, head):
            raise RuntimeError("graded SHA %s is not an ancestor of head %s" % (graded[:7], head[:7]))
        return first_parent_range(graded, head)
    created = iso(fail.get("created_at") or "")
    if not created:
        raise RuntimeError("FAIL comment has no graded SHA and no created_at")
    after = []
    for sha in first_parent_range(base, head):
        if commit_date(sha) > created:
            after.append(sha)
    return after


def unfixed_after(commits, base):
    remaining = []
    for sha in commits:
        if is_ancestor(sha, base):
            continue
        if is_main_merge(sha, base):
            continue
        remaining.append(sha)
    return remaining


def fail_label(fail):
    cid = fail.get("id")
    graded = sha_from_comment(fail.get("body") or "")
    parts = []
    if cid is not None:
        parts.append("id %s" % cid)
    if graded:
        parts.append("on %s" % graded[:7])
    if parts:
        return ", ".join(parts)
    return "no id"


def unfixed_fail_problems(comments, head, base):
    fail = newest_fail(comments)
    if not fail:
        return []
    after = commits_after_fail(fail, head, base)
    if unfixed_after(after, base):
        return []
    return [UNFIXED_FAIL % fail_label(fail)]


def problems(body, comments, head, base):
    out = []
    out.extend(proof_item_problems(body))
    out.extend(unfixed_fail_problems(comments, head, base))
    return out


def next_link(header):
    for part in (header or "").split(","):
        if 'rel="next"' not in part and "rel='next'" not in part:
            continue
        start = part.find("<")
        end = part.find(">", start)
        if start >= 0 and end > start:
            return part[start + 1:end].strip()
    return ""


def comment_headers(token):
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "mirc-sqlite-prior-fail-check",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = "Bearer %s" % token
    return headers


def fetch_comments(repo, number, token=""):
    url = "https://api.github.com/repos/%s/issues/%d/comments?per_page=100" % (repo, number)
    out = []
    while url:
        req = urllib.request.Request(url, headers=comment_headers(token))
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.load(resp)
                link = resp.headers.get("Link") or resp.headers.get("link") or ""
        except urllib.error.URLError as exc:
            raise RuntimeError(str(exc.reason if getattr(exc, "reason", None) else exc))
        if not isinstance(data, list):
            raise RuntimeError("comments response is not a list")
        out.extend(data)
        url = next_link(link)
    return out


def load_comments(path, repo, number):
    if path:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        if not isinstance(data, list):
            raise RuntimeError("comments file is not a list")
        return data
    token = (os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or "").strip()
    repo = (repo or os.environ.get("GITHUB_REPOSITORY") or "").strip()
    if not repo:
        raise RuntimeError("repository is unknown")
    return fetch_comments(repo, number, token)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fail a PROD PR re-asked after an unfixed Grade: FAIL.")
    ap.add_argument("--event", help="GitHub event JSON ($GITHUB_EVENT_PATH)")
    ap.add_argument("--comments", help="PR issue comments JSON list")
    a = ap.parse_args(argv)
    if not a.event:
        ap.error("pass --event")
    with open(a.event, encoding="utf-8") as fh:
        event = json.load(fh)
    pr = event.get("pull_request")
    if not pr:
        print("prior_fail_check: not a pull_request event; nothing to check")
        return 0
    body = pr.get("body") or ""
    base = ((pr.get("base") or {}).get("sha") or "").strip()
    head = ((pr.get("head") or {}).get("sha") or "").strip()
    if pr_body_check.tier_from_base_sha(base) != "PROD":
        print("prior_fail_check: not a PROD PR; nothing to check")
        return 0
    if not base or not head:
        print(GIT_FIX % "base or head SHA is missing")
        print(SAME_PR)
        return 1
    if not is_commit(base) or not is_commit(head):
        print(GIT_FIX % "base or head SHA is not in this checkout")
        print(SAME_PR)
        return 1
    repo = ((event.get("repository") or {}).get("full_name") or "").strip()
    number = pr.get("number")
    try:
        comments = load_comments(a.comments, repo, number)
    except (OSError, json.JSONDecodeError, RuntimeError, TypeError, ValueError) as exc:
        print(COMMENTS_FIX % exc)
        print(SAME_PR)
        return 1
    try:
        found = problems(body, comments, head, base)
    except RuntimeError as exc:
        print(GIT_FIX % exc)
        print(SAME_PR)
        return 1
    for line in found:
        print(line)
    if found:
        print(SAME_PR)
        return 1
    print("prior_fail_check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
