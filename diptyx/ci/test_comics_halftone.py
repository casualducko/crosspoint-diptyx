#!/usr/bin/env python3
"""python3 diptyx/ci/test_comics_halftone.py

Compiles the firmware's ComicsHalftone.cpp on this machine and requires its output to equal the independent Python
reference (comics_halftone_ref.py) bit for bit, over images of many sizes and kinds (noise, ramps, hard edges, a
comic-like page). Also checks the output is only 0 or 3 and that bad arguments are refused.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import comics_halftone_ref as ref  # noqa: E402

SRC = os.path.join(HERE, "..", "..", "lib", "Epub", "Epub", "converters")
TONE = [int(255.0 * (i / 255.0) ** 0.8 + 0.5) for i in range(256)]  # the firmware's bwToneCurve


def compile_cli(tmp):
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise unittest.SkipTest("no C++ compiler")
    exe = os.path.join(tmp, "halftone_cli")
    subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-I", SRC, "-o", exe,
                    os.path.join(HERE, "comics_halftone_cli.cpp"), os.path.join(SRC, "ComicsHalftone.cpp")],
                   check=True)
    return exe


def run_cli(exe, g):
    h, w = g.shape
    payload = f"{w} {h}\n".encode() + (" ".join(map(str, TONE)) + "\n").encode() + g.astype(np.uint8).tobytes()
    p = subprocess.run([exe], input=payload, capture_output=True)
    assert p.returncode == 0, f"cli exited {p.returncode} for {w}x{h}"
    return np.frombuffer(p.stdout, dtype=np.uint8).reshape(h, w)


def comic_like(w, h, seed):
    """Lettering-sized strokes, a screentone, a smooth ramp and hard black/white blocks on grey paper."""
    rng = np.random.default_rng(seed)
    g = np.full((h, w), 225, np.int64) + rng.integers(-6, 7, (h, w))
    for _ in range(max(3, w * h // 400)):
        y, x = rng.integers(0, h), rng.integers(0, w)
        g[y:y + 1 + rng.integers(0, 2), x:x + rng.integers(2, 9)] = 40  # short dark strokes
    g[h // 2:, : w // 3] = (np.indices((h - h // 2, w // 3)).sum(0) % 4 < 2) * 200 + 30  # screentone
    g[: h // 3, w // 2:] = np.linspace(20, 240, w - w // 2)[None, :]  # ramp
    g[h // 3:h // 2, w // 2:] = 0
    return np.clip(g, 0, 255).astype(np.uint8)


class ComicsHalftoneMatchesReference(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = compile_cli(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def check(self, g):
        got = run_cli(self.exe, g)
        want = ref.halftone(g, TONE)
        self.assertTrue(set(np.unique(got)) <= {0, 3})
        if not np.array_equal(got, want):
            ys, xs = np.nonzero(got != want)
            self.fail(f"{g.shape[1]}x{g.shape[0]}: {len(ys)} pixels differ, first at x={xs[0]} y={ys[0]}")

    def test_tiny_sizes(self):
        rng = np.random.default_rng(1)
        for h, w in [(1, 1), (1, 9), (9, 1), (2, 2), (3, 5), (5, 3), (6, 6), (7, 13), (13, 7), (14, 14)]:
            self.check(rng.integers(0, 256, (h, w)).astype(np.uint8))

    def test_odd_sizes_with_noise(self):
        rng = np.random.default_rng(2)
        for h, w in [(37, 23), (61, 100), (100, 61), (45, 45)]:
            self.check(rng.integers(0, 256, (h, w)).astype(np.uint8))

    def test_flat_levels(self):
        for v in (0, 1, 40, 128, 140, 200, 224, 255):
            self.check(np.full((20, 31), v, np.uint8))

    def test_ramps_and_hard_edges(self):
        ramp = np.tile(np.linspace(0, 255, 90).astype(np.uint8), (30, 1))
        self.check(ramp)
        self.check(ramp.T.copy())
        edges = np.zeros((40, 40), np.uint8)
        edges[:, 20:] = 255
        edges[10:30, 5:35] = 128
        self.check(edges)

    def test_comic_like_pages(self):
        for seed, (h, w) in enumerate([(120, 80), (200, 150), (97, 211)]):
            self.check(comic_like(w, h, seed))

    def test_threshold_boundaries(self):
        """Bright paper with black marks and pixels around the 150 cap of the local threshold: exercises the boundary."""
        rng = np.random.default_rng(5)
        for h, w in [(40, 60), (64, 64), (33, 90)]:
            pool = np.array([255, 255, 255, 240, 0, 0, 148, 149, 150, 150, 151, 152, 153, 200])
            self.check(rng.choice(pool, (h, w)).astype(np.uint8))

    def test_panel_sized_page(self):
        self.check(comic_like(432, 648, 9))

    def test_rejects_bad_arguments(self):
        payload = b"0 5\n" + (" ".join(map(str, TONE)) + "\n").encode()
        p = subprocess.run([self.exe], input=payload, capture_output=True)
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
