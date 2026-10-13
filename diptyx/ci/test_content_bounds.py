#!/usr/bin/env python3
"""python3 diptyx/ci/test_content_bounds.py

Compiles the firmware's ContentBounds.cpp (Trim Margins) on this machine and checks it on synthetic pages: content
with margins, off-white paper, scan dust, a dark page, a blank page, content that fills the page.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "lib", "Epub", "Epub", "converters")


def compile_cli(tmp):
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise unittest.SkipTest("no C++ compiler")
    exe = os.path.join(tmp, "bounds_cli")
    subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-I", SRC, "-o", exe,
                    os.path.join(HERE, "content_bounds_cli.cpp"), os.path.join(SRC, "ContentBounds.cpp")], check=True)
    return exe


def find(exe, g):
    h, w = g.shape
    p = subprocess.run([exe], input=f"{w} {h}\n".encode() + g.astype(np.uint8).tobytes(), capture_output=True)
    assert p.returncode == 0
    out = p.stdout.decode().split()
    return None if out == ["none"] else tuple(int(v) for v in out)


class ContentBoundsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.exe = compile_cli(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def page(self, paper=250, w=270, h=382):
        return np.full((h, w), paper, np.uint8)

    def test_margins_found_with_small_padding(self):
        g = self.page()
        g[60:300, 40:200] = 40
        x0, y0, x1, y1 = find(self.exe, g)
        self.assertTrue(30 <= x0 <= 40 and 56 <= y0 <= 60 - 0 and 200 <= x1 <= 212 and 300 <= y1 <= 308, (x0, y0, x1, y1))

    def test_off_white_paper(self):
        g = self.page(paper=205)
        g[100:250, 80:180] = 30
        r = find(self.exe, g)
        self.assertIsNotNone(r)
        self.assertTrue(r[0] <= 80 and r[2] >= 180 and r[1] <= 100 and r[3] >= 250 and r[2] - r[0] < 140)

    def test_dust_is_ignored(self):
        g = self.page()
        g[60:300, 40:200] = 40
        g[5, 5] = 0
        g[370, 260] = 0
        g[10, 100:102] = 0  # a speck two pixels wide
        x0, y0, x1, y1 = find(self.exe, g)
        self.assertGreater(y0, 40)
        self.assertLess(y1, 330)
        self.assertGreater(x0, 20)

    def test_dark_page_is_not_trimmed(self):
        self.assertIsNone(find(self.exe, self.page(paper=60)))

    def test_blank_page(self):
        self.assertIsNone(find(self.exe, self.page()))

    def test_content_to_the_page_edges_covers_everything(self):
        g = self.page()
        g[:4, :] = g[-4:, :] = 30  # a frame drawn to the page edges
        g[:, :4] = g[:, -4:] = 30
        x0, y0, x1, y1 = find(self.exe, g)
        self.assertEqual((x0, y0, x1, y1), (0, 0, 270, 382))

    def test_tiny_images_are_refused(self):
        self.assertIsNone(find(self.exe, np.zeros((4, 4), np.uint8)))


if __name__ == "__main__":
    unittest.main()
