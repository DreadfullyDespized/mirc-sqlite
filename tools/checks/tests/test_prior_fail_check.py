import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import prior_fail_check as C

PASSING_BODY = (
    "For Dread: Merge (PROD) after Hester's PASS.\n"
    "Issue: https://github.com/DreadfullyDespized/mirc-sqlite/issues/35\n"
    "Doer: Lyska\n"
    "Grader: Hester Quill\n"
    "Problem: a FAIL can be re-asked after only a main merge\n"
    "Fix: fail that case in CI\n"
    "Tested: python3 -m unittest discover -s tools/checks/tests -v\n"
    "Not checked / Risk: none\n"
)

PROOF_BODY = (
    "For Dread: Waive or run Proof item 2 in mIRC with the new DLL, then merge (PROD).\n"
    "Issue: https://github.com/DreadfullyDespized/mirc-sqlite/issues/15\n"
    "Doer: Lyska\n"
    "Grader: Hester Quill\n"
    "Problem: SQLite 3.53.4 is current\n"
    "Fix: pin 3.54.0\n"
    "Tested: python3 -m unittest discover -s tools/checks/tests -v (82 OK)\n"
    "Not checked / Risk: Proof item 2's real-mIRC run is pending.\n"
)

PROOF_WITH_LOG = (
    "For Dread: Merge (PROD) after Hester's PASS.\n"
    "Issue: https://github.com/DreadfullyDespized/mirc-sqlite/issues/15\n"
    "Doer: Lyska\n"
    "Grader: Hester Quill\n"
    "Problem: SQLite 3.53.4 is current\n"
    "Fix: pin 3.54.0\n"
    "Tested: real-mIRC proof log https://example.test/proof-3540.log (all steps PASS)\n"
    "Not checked / Risk: none\n"
)

ISSUE_30_BODY = (
    "For Dread: Waive or run Proof item 2 in mIRC with the new DLL (version 3.54.0, "
    "create_function, nested-call error 200), then merge (PROD) after Hester's PASS.\n"
    "Issue: https://github.com/DreadfullyDespized/mirc-sqlite/issues/15\n"
    "Doer: Lyska\n"
    "Grader: Hester Quill\n"
    "Problem: msqlite.dll builds against SQLite 3.53.4; 3.54.0 is out.\n"
    "Fix: pin 3.54.0 and DLL 2.1.0; add proof s16 AUTHFN, s17 create_function, and s18 "
    "nested-call error 200; teach diff.sh to accept only AUTHFN-DENY create=1 errno=0 vs "
    "create=0 errno=23.\n"
    "Tested: python3 -m unittest discover -s tools/checks/tests -v (82 OK); no_new_comments "
    "--all OK; bash tools/proof/diff.sh: known AUTHFN-DENY pair PASS, new-side create=1 FAIL, "
    "errno=19 FAIL, missing line FAIL.\n"
    "Not checked / Risk: Proof item 2's real-mIRC run is pending Dread's waiver or run. If "
    "3.54.0's DEFAULT-function authorizer change trips a bot script, that script's CREATE "
    "TABLE fails at load.\n"
)

FAIL_ON = (
    "Grade: FAIL - (1) issue #15 Proof item 2 needs a real-mIRC run; (2) proof.mrc has no "
    "create_function step. On %s.\n"
    "Gates: cross-compile green (run 43330736306); pii-gate green (run 43330909582).\n"
    "For Dread: nothing yet.\n"
)


def run_cli(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = C.main(list(argv))
    return rc, out.getvalue()


def git(repo, *args):
    proc = subprocess.run(["git", *args], cwd=repo, capture_output=True)
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(
            proc.returncode, proc.args, output=proc.stdout, stderr=proc.stderr
        )


def sha(repo, ref="HEAD"):
    return subprocess.check_output(["git", "rev-parse", ref], cwd=repo, text=True).strip()


def write_readme(repo, tier):
    path = os.path.join(repo, "README.md")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("Repo tier: %s\n" % tier)
    git(repo, "add", "README.md")


class Repo:
    def __init__(self, tier="PROD"):
        self.path = tempfile.mkdtemp(prefix="prior_fail_")
        git(self.path, "init")
        git(self.path, "config", "user.name", "Lyska")
        git(self.path, "config", "user.email", "5559848+DreadfullyDespized@users.noreply.github.com")
        git(self.path, "config", "commit.gpgsign", "false")
        write_readme(self.path, tier)
        git(self.path, "commit", "-m", "base")
        git(self.path, "branch", "-M", "main")
        self.base = sha(self.path)

    def commit(self, message, filename=None, text=None):
        filename = filename or (message.replace(" ", "-") + ".txt")
        path = os.path.join(self.path, filename)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(text if text is not None else message + "\n")
        git(self.path, "add", filename)
        git(self.path, "commit", "-m", message)
        return sha(self.path)

    def merge_main(self):
        git(self.path, "merge", "main", "-m", "Merge branch 'main' into feature")
        return sha(self.path)

    def event(self, body, head=None, number=35):
        head = head or sha(self.path)
        return {
            "pull_request": {
                "number": number,
                "body": body,
                "base": {"sha": self.base},
                "head": {"sha": head},
            },
            "repository": {"full_name": "DreadfullyDespized/mirc-sqlite"},
        }

    def close(self):
        shutil.rmtree(self.path, ignore_errors=True)


class PriorFailCheck(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="prior_fail_cli_")
        self.repo = Repo()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        self.repo.close()

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        if isinstance(data, (dict, list)):
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(data, fh)
        else:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(data)
        return path

    def fail_comment(self, on_sha, created="2026-11-13T13:59:58Z", cid=6855099243):
        return {
            "id": cid,
            "created_at": created,
            "body": FAIL_ON % on_sha,
        }

    def run_on(self, body, comments, head=None):
        event = self.write("event.json", self.repo.event(body, head=head))
        comments_path = self.write("comments.json", comments)
        old = os.getcwd()
        try:
            os.chdir(self.repo.path)
            return run_cli("--event", event, "--comments", comments_path)
        finally:
            os.chdir(old)

    def test_not_a_pull_request_event_ok(self):
        event = self.write("event.json", {"ref": "refs/heads/main"})
        rc, out = run_cli("--event", event)
        self.assertEqual(rc, 0, out)
        self.assertIn("not a pull_request event", out)

    def test_test_tier_skips(self):
        self.repo.close()
        self.repo = Repo(tier="TEST")
        graded = self.repo.commit("feature")
        git(self.repo.path, "checkout", "-b", "feature")
        rc, out = self.run_on(PROOF_BODY, [self.fail_comment(graded)])
        self.assertEqual(rc, 0, out)
        self.assertIn("not a PROD PR", out)

    def test_clean_prod_pr_ok(self):
        git(self.repo.path, "checkout", "-b", "feature")
        self.repo.commit("feature")
        rc, out = self.run_on(PASSING_BODY, [])
        self.assertEqual(rc, 0, out)
        self.assertIn("prior_fail_check: OK", out)

    def test_fail_then_only_main_merge_fails(self):
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        git(self.repo.path, "checkout", "main")
        main_commit = self.repo.commit("main work")
        self.repo.base = main_commit
        git(self.repo.path, "checkout", "feature")
        merged = self.repo.merge_main()
        rc, out = self.run_on(PASSING_BODY, [self.fail_comment(graded)], head=merged)
        self.assertEqual(rc, 1, out)
        self.assertIn("no later commit besides main merges", out)
        self.assertIn("id 6855099243", out)
        self.assertIn("on %s" % graded[:7], out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))

    def test_fail_then_fix_commit_passes_rule_one(self):
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        git(self.repo.path, "checkout", "main")
        main_commit = self.repo.commit("main work")
        self.repo.base = main_commit
        git(self.repo.path, "checkout", "feature")
        self.repo.merge_main()
        self.repo.commit("add proof steps")
        rc, out = self.run_on(PASSING_BODY, [self.fail_comment(graded)])
        self.assertEqual(rc, 0, out)
        self.assertIn("prior_fail_check: OK", out)

    def test_fail_with_no_later_commit_fails(self):
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        rc, out = self.run_on(PASSING_BODY, [self.fail_comment(graded)])
        self.assertEqual(rc, 1, out)
        self.assertIn("no later commit besides main merges", out)

    def test_run_ids_are_not_used_as_the_graded_sha(self):
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        comment = self.fail_comment(graded)
        self.assertIn("43330736306", comment["body"])
        self.assertEqual(C.sha_from_comment(comment["body"]), "")
        old = os.getcwd()
        try:
            os.chdir(self.repo.path)
            self.assertEqual(C.sha_from_comment(comment["body"]), graded)
        finally:
            os.chdir(old)
        rc, out = self.run_on(PASSING_BODY, [comment])
        self.assertEqual(rc, 1, out)
        self.assertIn("on %s" % graded[:7], out)

    def test_time_fallback_when_comment_has_no_sha(self):
        git(self.repo.path, "checkout", "-b", "feature")
        self.repo.commit("feature")
        later = "2099-01-01T00:00:00Z"
        comment = {"id": 1, "created_at": later, "body": "Grade: FAIL - blockers still open.\n"}
        rc, out = self.run_on(PASSING_BODY, [comment])
        self.assertEqual(rc, 1, out)
        self.assertIn("no later commit besides main merges", out)

    def test_newest_fail_is_used(self):
        git(self.repo.path, "checkout", "-b", "feature")
        first = self.repo.commit("feature")
        git(self.repo.path, "checkout", "main")
        self.repo.base = self.repo.commit("main work")
        git(self.repo.path, "checkout", "feature")
        self.repo.merge_main()
        second = self.repo.commit("unrelated")
        comments = [
            self.fail_comment(first, created="2026-11-13T13:59:58Z", cid=1),
            self.fail_comment(second, created="2026-11-16T23:33:00Z", cid=2),
        ]
        rc, out = self.run_on(PASSING_BODY, comments)
        self.assertEqual(rc, 1, out)
        self.assertIn("id 2", out)
        self.assertIn("on %s" % second[:7], out)

    def test_proof_item_inside_html_comment_is_ignored(self):
        hidden = "<!-- Proof item stays in the comment -->\n"
        self.assertEqual(C.proof_item_problems(hidden), [])
        visible = hidden + "Proof item is outside the comment\n"
        self.assertEqual(C.proof_item_problems(visible), [C.PROOF_NO_LOG])

    def test_proof_item_without_log_fails(self):
        git(self.repo.path, "checkout", "-b", "feature")
        self.repo.commit("feature")
        rc, out = self.run_on(PROOF_BODY, [])
        self.assertEqual(rc, 1, out)
        self.assertIn("names a Proof item and has no linked live log", out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))

    def test_issue_url_line_is_not_a_live_log(self):
        self.assertFalse(C.has_linked_live_log(PROOF_BODY))
        self.assertTrue(C.has_linked_live_log(PROOF_WITH_LOG))

    def test_proof_item_with_log_passes(self):
        git(self.repo.path, "checkout", "-b", "feature")
        self.repo.commit("feature")
        rc, out = self.run_on(PROOF_WITH_LOG, [])
        self.assertEqual(rc, 0, out)
        self.assertIn("prior_fail_check: OK", out)

    def test_issue_30_body_fails_proof_rule(self):
        self.assertTrue(C.names_proof_item(ISSUE_30_BODY))
        self.assertFalse(C.has_linked_live_log(ISSUE_30_BODY))
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        git(self.repo.path, "checkout", "main")
        self.repo.base = self.repo.commit("main work")
        git(self.repo.path, "checkout", "feature")
        self.repo.merge_main()
        self.repo.commit("add proof steps")
        rc, out = self.run_on(ISSUE_30_BODY, [self.fail_comment(graded)])
        self.assertEqual(rc, 1, out)
        self.assertIn("names a Proof item and has no linked live log", out)
        self.assertNotIn("no later commit besides main merges", out)

    def test_quoted_grade_fail_is_ignored(self):
        git(self.repo.path, "checkout", "-b", "feature")
        graded = self.repo.commit("feature")
        comment = {
            "id": 9,
            "created_at": "2026-11-16T00:00:00Z",
            "body": "> Grade: FAIL - old blockers. On %s.\nThis is a note, not a grade.\n" % graded,
        }
        rc, out = self.run_on(PASSING_BODY, [comment])
        self.assertEqual(rc, 0, out)
        self.assertIn("prior_fail_check: OK", out)

    def test_missing_comments_file_fails_closed(self):
        event = self.write("event.json", self.repo.event(PASSING_BODY))
        old = os.getcwd()
        try:
            os.chdir(self.repo.path)
            rc, out = run_cli("--event", event, "--comments", os.path.join(self.dir, "missing.json"))
        finally:
            os.chdir(old)
        self.assertEqual(rc, 1, out)
        self.assertIn("could not read the PR comments", out)


if __name__ == "__main__":
    unittest.main()
