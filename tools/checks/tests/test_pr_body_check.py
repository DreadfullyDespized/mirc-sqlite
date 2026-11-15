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
import pr_body_check as C

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TEMPLATE = os.path.join(ROOT, ".github", "pull_request_template.md")
TEMPLATE_TEXT = (
    "For Dread:\n"
    "Closes #\n"
    "\n"
    "Doer:\n"
    "Grader:\n"
    "\n"
    "Problem:\n"
    "Fix:\n"
    "Tested:\n"
    "Not checked / Risk:\n"
)
PASSING = (
    "For Dread: shared body check\n"
    "Closes #31\n"
    "\n"
    "Doer: Lyska\n"
    "Grader: Hester Quill\n"
    "\n"
    "Problem: the old body section is banned\n"
    "Fix: enforce the repo-loop body\n"
    "Tested: python3 -m unittest discover -s tools/checks/tests -v\n"
    "Not checked / Risk: none\n"
)


def run_cli(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = C.main(list(argv))
    return rc, out.getvalue()


def padded(extra):
    lines = [
        "For Dread: length check",
        "Closes #31",
        "Doer: Lyska",
        "Grader: Hester Quill",
        "Problem: length",
        "Fix: trim",
        "Tested: unittest ok",
        "Not checked / Risk: none",
    ]
    lines.extend(extra)
    return "\n".join(lines) + "\n"


class TemplateShape(unittest.TestCase):
    def test_template_file_is_the_repo_loop_lines(self):
        with open(TEMPLATE, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), TEMPLATE_TEXT)

    def test_passing_body_in_the_template_shape(self):
        self.assertEqual(C.problems(PASSING), [])
        rc, out = run_cli("--body-file", self.write("ok.md", PASSING))
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: OK", out)

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="pr_body_")


class Rules(unittest.TestCase):
    def test_a_first_line_must_be_for_dread(self):
        body = PASSING.replace("For Dread: shared body check\n", "Hello\n")
        errs = C.problems(body)
        self.assertEqual(len(errs), 1)
        self.assertIn("For Dread", errs[0])
        self.assertIn("Fix:", errs[0])

    def test_b_more_than_twelve_nonblank_lines_fails(self):
        errs = C.problems(padded(["n1", "n2", "n3", "n4", "n5"]))
        self.assertEqual(len(errs), 1)
        self.assertIn("13", errs[0])
        self.assertIn("12", errs[0])
        self.assertIn("Fix:", errs[0])

    def test_c_doer_and_grader_must_be_current_seats(self):
        body = PASSING.replace("Doer: Lyska\n", "Doer: Rei\n")
        errs = C.problems(body)
        self.assertEqual(len(errs), 1)
        self.assertIn("Rei", errs[0])
        self.assertIn("Fix:", errs[0])

    def test_d_tested_must_have_text(self):
        body = PASSING.replace(
            "Tested: python3 -m unittest discover -s tools/checks/tests -v\n",
            "Tested:\n",
        )
        errs = C.problems(body)
        self.assertEqual(len(errs), 1)
        self.assertIn("Tested:", errs[0])
        self.assertIn("Fix:", errs[0])
        self.assertTrue(C.problems(PASSING.replace("Tested:", "Note:")))

    def test_retired_names_fail(self):
        for doer, grader in (
            ("Rei", "Hester"),
            ("Lyska", "Priya"),
            ("Lyska Embervane", "Hester"),
        ):
            body = "For Dread: seats\nDoer: %s\nGrader: %s\nTested: ran the suite\n" % (doer, grader)
            errs = C.problems(body)
            self.assertTrue(errs, (doer, grader))
            self.assertTrue(any("not a current seat" in e for e in errs), errs)

    def test_twelve_line_body_passes(self):
        body = padded(["n1", "n2", "n3", "n4"])
        self.assertEqual(len([ln for ln in body.split("\n") if ln.strip()]), 12)
        self.assertEqual(C.problems(body), [])

    def test_thirteen_line_body_fails(self):
        body = padded(["n1", "n2", "n3", "n4", "n5"])
        self.assertEqual(len([ln for ln in body.split("\n") if ln.strip()]), 13)
        self.assertTrue(C.problems(body))

    def test_self_merge_grader_passes_and_same_person_fails(self):
        ok = "For Dread: no grade needed\nDoer: Lyska\nGrader: self-merge\nTested: ran\n"
        self.assertEqual(C.problems(ok), [])
        self.assertEqual(C.problems(ok.replace("self-merge", "Self-Merge")), [])
        same = "For Dread: same person\nDoer: Lyska\nGrader: lyska\nTested: ran\n"
        errs = C.problems(same)
        self.assertTrue(any("same" in e for e in errs), errs)

    def test_parenthetical_and_case_are_stripped(self):
        body = (
            "For Dread: paren\n"
            "Doer: Lyska (seat)\n"
            "Grader: Hester Quill (not the doer)\n"
            "Tested: ran\n"
        )
        self.assertEqual(C.problems(body), [])
        body = "For Dread: case\n  doer: calla\n  grader: ines varro\nTested: ran\n"
        self.assertEqual(C.problems(body), [])

    def test_html_comments_and_cr_are_stripped_first(self):
        hidden = "<!--\nFor Dread: hidden\n-->\nCloses #1\nDoer: Lyska\nGrader: Hester\nTested: ran\n"
        self.assertTrue(any("For Dread" in e for e in C.problems(hidden)))
        commented = PASSING + "<!--\n" + ("extra line\n" * 20) + "-->\n"
        self.assertEqual(C.problems(commented), [])
        crlf = PASSING.replace("\n", "\r\n")
        self.assertEqual(C.problems(crlf), [])
        open_comment = "For Dread: ok\nDoer: Lyska\nGrader: Hester\nTested: ran\n<!-- unterminated\nmore\n"
        self.assertEqual(C.problems(open_comment), [])


class Cli(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="pr_body_cli_")

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path

    def event(self, payload):
        return self.write("event.json", json.dumps(payload))

    def test_non_pr_event_exits_0(self):
        rc, out = run_cli("--event", self.event({}))
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: not a pull_request event; nothing to check", out)
        rc, out = run_cli("--event", self.event({"push": {"ref": "refs/heads/main"}}))
        self.assertEqual(rc, 0, out)

    def test_event_body_pass_and_fail(self):
        rc, out = run_cli("--event", self.event({"pull_request": {"body": PASSING}}))
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: OK", out)
        bad = PASSING.replace("For Dread: shared body check\n", "")
        rc, out = run_cli("--event", self.event({"pull_request": {"body": bad}}))
        self.assertEqual(rc, 1, out)
        self.assertIn("Fix:", out)
        self.assertIn(C.SAME_PR, out)
        rc, out = run_cli("--event", self.event({"pull_request": {"body": None}}))
        self.assertEqual(rc, 1, out)

    def test_failure_names_every_problem_and_ends_with_the_same_pr_line(self):
        rc, out = run_cli("--body-file", self.write("bad.md", "Hello\n"))
        self.assertEqual(rc, 1, out)
        self.assertGreater(out.count("pr_body_check:"), 1)
        self.assertTrue(out.strip().endswith(C.SAME_PR))



SELF_MERGE_BODY = "For Dread: no grade needed\nDoer: Lyska\nGrader: self-merge\nTested: ran the suite\n"
HESTER_BODY = "For Dread: named grader\nDoer: Lyska\nGrader: Hester Quill\nTested: ran the suite\n"
PROD_SELF_MERGE_MSG = (
    "pr_body_check: Grader: self-merge is not allowed on a PROD repo (README says Repo tier: PROD). "
    "Fix: set Grader: Hester Quill (a named current seat) in the PR body."
)


class Tier(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="pr_body_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path

    def check(self, readme_text, body):
        return run_cli(
            "--body-file",
            self.write("body.md", body),
            "--readme",
            self.write("README.md", readme_text),
        )

    def test_prod_readme_self_merge_fails(self):
        rc, out = self.check("**Repo label: PROD**\nRepo tier: PROD\n", SELF_MERGE_BODY)
        self.assertEqual(rc, 1, out)
        self.assertIn(PROD_SELF_MERGE_MSG, out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))
        rc, out = self.check("Repo tier: PROD\n", SELF_MERGE_BODY.replace("self-merge", "Self-Merge"))
        self.assertEqual(rc, 1, out)
        self.assertIn(PROD_SELF_MERGE_MSG, out)

    def test_test_readme_self_merge_passes(self):
        rc, out = self.check("**Repo label: PROD**\nRepo tier: TEST\n", SELF_MERGE_BODY)
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: OK", out)
        self.assertNotIn("self-merge is not allowed", out)

    def test_prod_readme_hester_quill_passes(self):
        rc, out = self.check("Repo tier: PROD\n", HESTER_BODY)
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: OK", out)

    def test_no_tier_line_self_merge_fails(self):
        rc, out = self.check("# About\nThis file does not name a tier.\n", SELF_MERGE_BODY)
        self.assertEqual(rc, 1, out)
        self.assertIn(PROD_SELF_MERGE_MSG, out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))

    def test_repo_label_prod_only_self_merge_fails(self):
        rc, out = self.check("**Repo label: PROD** — see CONTRIBUTING.md.\n", SELF_MERGE_BODY)
        self.assertEqual(rc, 1, out)
        self.assertIn(PROD_SELF_MERGE_MSG, out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))

    def test_default_readme_is_repo_root_next_to_tools(self):
        path = C.default_readme()
        root = os.path.dirname(path)
        self.assertEqual(os.path.basename(path), "README.md")
        self.assertTrue(os.path.isdir(os.path.join(root, "tools")))
        self.assertTrue(os.path.isfile(path))
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        self.assertRegex(text, r"(?m)^Repo tier:\s*PROD\s*$")
        self.assertEqual(C.tier_from_text(text), "PROD")

    def test_event_without_readme_flag_uses_repo_readme(self):
        event = self.write("event.json", json.dumps({"pull_request": {"body": SELF_MERGE_BODY}}))
        rc, out = run_cli("--event", event)
        self.assertEqual(rc, 1, out)
        self.assertIn(PROD_SELF_MERGE_MSG, out)
        self.assertTrue(out.strip().endswith(C.SAME_PR))
        event = self.write("event.json", json.dumps({"pull_request": {"body": HESTER_BODY}}))
        rc, out = run_cli("--event", event)
        self.assertEqual(rc, 0, out)
        self.assertIn("pr_body_check: OK", out)

    def test_base_prod_head_test_readme_self_merge_fails(self):
        repo = tempfile.mkdtemp(prefix="pr_body_git_")
        try:
            def git(*args):
                subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)

            git("init")
            git("config", "user.name", "Lyska")
            git("config", "user.email", "5559848+DreadfullyDespized@users.noreply.github.com")
            git("config", "commit.gpgsign", "false")
            readme = os.path.join(repo, "README.md")
            with open(readme, "w", encoding="utf-8") as fh:
                fh.write("Repo tier: PROD\n")
            git("add", "README.md")
            git("commit", "-m", "base prod")
            base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
            with open(readme, "w", encoding="utf-8") as fh:
                fh.write("Repo tier: TEST\n")
            git("add", "README.md")
            git("commit", "-m", "head test")
            event = os.path.join(repo, "event.json")
            with open(event, "w", encoding="utf-8") as fh:
                json.dump({"pull_request": {"body": SELF_MERGE_BODY, "base": {"sha": base}}}, fh)
            old = os.getcwd()
            try:
                os.chdir(repo)
                rc, out = run_cli("--event", event)
            finally:
                os.chdir(old)
            self.assertEqual(rc, 1, out)
            self.assertIn(PROD_SELF_MERGE_MSG, out)
            self.assertTrue(out.strip().endswith(C.SAME_PR))
        finally:
            shutil.rmtree(repo, ignore_errors=True)

if __name__ == "__main__":
    unittest.main()
