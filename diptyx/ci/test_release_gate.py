#!/usr/bin/env python3
"""python3 diptyx/ci/test_release_gate.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_gate as g  # noqa: E402


def rel(tag, pre=False, draft=False):
    return {"tagName": tag, "isPrerelease": pre, "isDraft": draft}


PUBLISHED = [rel(f"diptyx-v1.6.5-{n}") for n in (1, 2, 9, 10)] + [rel("diptyx-v1.6.5-rc1", pre=True)]


class Decide(unittest.TestCase):
    def test_next_build_is_latest_and_deploys(self):
        self.assertEqual(g.decide("diptyx-v1.6.5-11", PUBLISHED), {"prerelease": False, "latest": True, "pages": True})

    def test_builds_compare_numerically_not_as_text(self):
        self.assertTrue(g.decide("diptyx-v1.6.5-10", [rel("diptyx-v1.6.5-9")])["latest"])
        self.assertFalse(g.decide("diptyx-v1.6.5-9", [rel("diptyx-v1.6.5-10")])["latest"])

    def test_older_tag_is_not_latest_and_leaves_pages_alone(self):
        out = g.decide("diptyx-v1.6.5-3", PUBLISHED)
        self.assertEqual(out, {"prerelease": False, "latest": False, "pages": False})

    def test_higher_upstream_release_beats_a_higher_build(self):
        self.assertTrue(g.decide("diptyx-v1.7.0-1", PUBLISHED)["latest"])
        self.assertFalse(g.decide("diptyx-v1.6.9-99", PUBLISHED + [rel("diptyx-v1.7.0-1")])["latest"])

    def test_rerun_of_the_newest_tag_still_deploys(self):
        self.assertTrue(g.decide("diptyx-v1.6.5-10", PUBLISHED)["pages"])

    def test_prerelease_tags_never_become_latest(self):
        for tag in ("diptyx-v1.6.6-rc1", "diptyx-v1.6.6-beta2", "diptyx-v1.6.6-dev"):
            self.assertEqual(g.decide(tag, PUBLISHED), {"prerelease": True, "latest": False, "pages": False})

    def test_prereleases_and_drafts_do_not_block_a_stable_release(self):
        others = [rel("diptyx-v2.0.0-1", pre=True), rel("diptyx-v2.0.0-2", draft=True)]
        self.assertTrue(g.decide("diptyx-v1.6.5-11", PUBLISHED + others)["latest"])

    def test_first_release(self):
        self.assertTrue(g.decide("diptyx-v1.6.5-1", [])["pages"])

    def test_malformed_tags_are_refused(self):
        for tag in ("diptyx-v1.6", "diptyx-v1.6.5", "diptyx-v1.6.5-", "diptyx-v1.6.5-1x", "diptyx-vX",
                    "diptyx-v1.6.5-1-extra", "diptyx-v1.6.5-rc1; echo", "diptyx-v1.6.5-1\n"):
            with self.assertRaises(ValueError, msg=tag):
                g.decide(tag, PUBLISHED)

    def test_every_existing_tag_is_accepted(self):
        for n in range(1, 11):
            g.decide(f"diptyx-v1.6.5-{n}", [])


if __name__ == "__main__":
    unittest.main()
