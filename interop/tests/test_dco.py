from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_dco  # noqa: E402


def commit(
    message: str,
    *,
    name: str = "Example Contributor",
    email: str = "contributor@example.com",
) -> check_dco.Commit:
    return check_dco.Commit(
        oid="0123456789abcdef",
        author_name=name,
        author_email=email,
        message=message,
    )


class DcoCheckTests(unittest.TestCase):
    def test_accepts_matching_author_signoff(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Implement the change\n\n"
                "Signed-off-by: Example Contributor "
                "<contributor@example.com>\n"
            )
        )

        self.assertIsNone(error)

    def test_rejects_missing_signoff(self) -> None:
        error = check_dco.validation_error(commit("Implement the change\n"))

        self.assertIn("missing", error or "")

    def test_rejects_another_contributors_signoff(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Implement the change\n\n"
                "Signed-off-by: Another Person <another@example.com>\n"
            )
        )

        self.assertIn("matching the commit author", error or "")

    def test_rejects_signoff_used_as_subject(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Signed-off-by: Example Contributor "
                "<contributor@example.com>\n"
            )
        )

        self.assertIn("missing", error or "")

    def test_rejects_signoff_outside_final_trailer_block(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Implement the change\n\n"
                "Signed-off-by: Example Contributor "
                "<contributor@example.com>\n\n"
                "Additional body text after the purported trailer.\n"
            )
        )

        self.assertIn("missing", error or "")

    def test_matches_identity_case_insensitively(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Implement the change\n\n"
                "Signed-off-by: example contributor "
                "<CONTRIBUTOR@EXAMPLE.COM>\n"
            )
        )

        self.assertIsNone(error)

    def test_requires_explicit_trusted_dependabot_policy(self) -> None:
        candidate = commit(
            "Bump dependency\n",
            name="dependabot[bot]",
            email=(
                "49699333+dependabot[bot]@users.noreply.github.com"
            ),
        )

        self.assertIn(
            "missing", check_dco.validation_error(candidate) or ""
        )
        error = check_dco.validation_error(
            candidate,
            allow_dependabot=True,
        )

        self.assertIsNone(error)

    def test_does_not_exempt_spoofed_dependabot_name(self) -> None:
        error = check_dco.validation_error(
            commit(
                "Bump dependency\n",
                name="dependabot[bot]",
                email="human@example.com",
            ),
            allow_dependabot=True,
        )

        self.assertIn("missing", error or "")


if __name__ == "__main__":
    unittest.main()
