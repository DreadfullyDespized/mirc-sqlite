import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DIFF = os.path.join(ROOT, "tools", "proof", "diff.sh")

SHARED = (
    "BEGIN variant=new mirc=7.79 dllver=2.1.0 libver=3.54.0\n"
    "OPEN file ok=1 errno=0\n"
    "AUTHFN set=1 create=1 insert=1 deflen=19\n"
    "AUTHFN clear=1\n"
    "CREATEFN set=1 ok=1 v=42 errno=0\n"
    "NESTED set=1 outer=1 v=7 nestok=0 errno=200\n"
    "END\n"
)

OLD_DENY = "AUTHFN-DENY create=1 errno=0\n"
NEW_DENY = "AUTHFN-DENY create=0 errno=23\n"


def write_log(dirpath, name, body):
    path = os.path.join(dirpath, name)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(body)
    return path


def with_deny(body, line):
    return body.replace("END\n", line + "END\n")


class ProofDiff(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_diff(self, old, new):
        return subprocess.run(
            ["bash", DIFF, old, new],
            capture_output=True,
            text=True,
        )

    def test_known_authfn_deny_pair_passes(self):
        old = write_log(self.dir, "old.log", with_deny(SHARED, OLD_DENY))
        new = write_log(self.dir, "new.log", with_deny(SHARED, NEW_DENY))
        r = self.run_diff(old, new)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PROOF-DIFF PASS", r.stdout)

    def test_new_side_create_1_fails(self):
        old = write_log(self.dir, "old.log", with_deny(SHARED, OLD_DENY))
        new = write_log(self.dir, "new.log", with_deny(SHARED, OLD_DENY))
        r = self.run_diff(old, new)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("PROOF-DIFF FAIL", r.stderr)

    def test_unexpected_errno_fails(self):
        old = write_log(self.dir, "old.log", with_deny(SHARED, OLD_DENY))
        new = write_log(
            self.dir,
            "new.log",
            with_deny(SHARED, "AUTHFN-DENY create=0 errno=19\n"),
        )
        r = self.run_diff(old, new)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("PROOF-DIFF FAIL", r.stderr)

    def test_authfn_deny_missing_on_one_side_fails(self):
        old = write_log(self.dir, "old.log", SHARED)
        new = write_log(self.dir, "new.log", with_deny(SHARED, NEW_DENY))
        r = self.run_diff(old, new)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("PROOF-DIFF FAIL", r.stderr)

    def test_unexpected_line_still_fails(self):
        old = write_log(self.dir, "old.log", with_deny(SHARED, OLD_DENY))
        new_body = with_deny(SHARED, NEW_DENY).replace(
            "OPEN file ok=1 errno=0\n", "OPEN file ok=0 errno=14\n"
        )
        new = write_log(self.dir, "new.log", new_body)
        r = self.run_diff(old, new)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("PROOF-DIFF FAIL", r.stderr)


if __name__ == "__main__":
    unittest.main()
