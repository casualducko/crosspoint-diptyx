#!/usr/bin/env python3
"""Release gate for .github/workflows/diptyx-release.yml.

  release_gate.py <tag> <releases.json>

<releases.json> is `gh release list --json tagName,isPrerelease,isDraft`. Prints GITHUB_OUTPUT lines:

  prerelease=true|false   the tag has an -rc / -beta / -dev suffix: a GitHub pre-release, never "latest", no Pages deploy
  latest=true|false       a stable tag that is not older than any published stable release: the one "latest" may be
  pages=true|false        same as latest: only the newest stable release may replace the public web flasher and the
                          manifest the on-device update check reads

and exits 1, printing why, when the tag is not `diptyx-v<major>.<minor>.<patch>-<build>` or a
-rc / -beta / -dev suffix (optionally numbered) in place of the build number.
"""
import json
import re
import sys

STABLE = re.compile(r"^diptyx-v(\d+)\.(\d+)\.(\d+)-(\d+)\Z")
PRE = re.compile(r"^diptyx-v(\d+)\.(\d+)\.(\d+)-(?:rc|beta|dev)\d*\Z")


def stable_key(tag):
    m = STABLE.match(tag)
    return tuple(int(x) for x in m.groups()) if m else None


def decide(tag, releases):
    """Returns {'prerelease': bool, 'latest': bool, 'pages': bool}; raises ValueError for an unusable tag."""
    if PRE.match(tag):
        return {"prerelease": True, "latest": False, "pages": False}
    key = stable_key(tag)
    if key is None:
        raise ValueError(f"tag {tag!r} is not diptyx-v<major>.<minor>.<patch>-<build> (build a number), "
                         "nor a -rc/-beta/-dev pre-release; refusing to publish it")
    others = [stable_key(r["tagName"]) for r in releases
              if not r.get("isDraft") and not r.get("isPrerelease") and r["tagName"] != tag]
    newest = all(k is None or k <= key for k in others)
    return {"prerelease": False, "latest": newest, "pages": newest}


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[2]) as f:
        releases = json.load(f)
    try:
        out = decide(argv[1], releases)
    except ValueError as e:
        print(f"::error::{e}", file=sys.stderr)
        return 1
    if not out["latest"] and not out["prerelease"]:
        print(f"::warning::{argv[1]} is older than a published release: it will not become 'latest' and the web "
              "flasher / update manifest are left alone", file=sys.stderr)
    for k, v in out.items():
        print(f"{k}={'true' if v else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
