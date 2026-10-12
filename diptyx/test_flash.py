#!/usr/bin/env python3
"""Tests for how flash.py leaves the chip: python3 diptyx/test_flash.py

esptool is replaced by a fake `subprocess.run`, so nothing touches a device. They check which esptool commands each
exit path issues: a run that ends without a write must restart the installed app with the RTC watchdog reset, a run
that never reached the chip must not try again, and the write itself must not be followed by a second reset.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash  # noqa: E402

SUBCOMMANDS = ("flash-id", "read-flash", "read-mac", "write-flash")
PTABLE = b"\x55" * flash.PTABLE_LEN
GOOD_ID = "Chip type: ESP32-S3 (QFN56)\nMAC: aa:bb:cc:dd:ee:ff\nDetected flash size: 16MB\n"


class FakeEsptool:
    """Stands in for subprocess.run: records every esptool command line and answers like a connected Diptyx."""

    def __init__(self):
        self.calls = []
        self.rc = {}            # sub-command -> return code
        self.flash_id = GOOD_ID
        self.ptable = PTABLE

    def __call__(self, cmd, **kwargs):
        sub = next(c for c in cmd if c in SUBCOMMANDS)
        self.calls.append(cmd)
        rc = self.rc.get(sub, 0)
        out = ""
        if sub == "flash-id":
            out = self.flash_id
        elif sub == "read-flash" and rc == 0:
            dest = cmd[-1]
            with open(dest, "wb") as f:
                f.write(self.ptable if "0x8000" in cmd or hex(flash.PTABLE_OFFSET) in cmd else b"backup")
        return subprocess.CompletedProcess(cmd, rc, out, "")

    def subs(self):
        return [next(c for c in cmd if c in SUBCOMMANDS) for cmd in self.calls]

    def resets(self):
        """The read-mac calls that restart the app (--after watchdog-reset)."""
        return [c for c in self.calls if "read-mac" in c and "watchdog-reset" in c]

    def writes(self):
        return [c for c in self.calls if "write-flash" in c]


class FlasherExits(unittest.TestCase):
    def setUp(self):
        self.fake = FakeEsptool()
        flash._in_bootloader = False
        patches = [mock.patch.object(flash.subprocess, "run", self.fake),
                   mock.patch.object(flash, "find_port", lambda wanted: "PORT"),
                   mock.patch.object(flash, "STOCK_PTABLE_SHA256", flash.hashlib.sha256(PTABLE).hexdigest()),
                   mock.patch.object(flash, "check_app_image", lambda path: 1234)]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, True))
        self.firmware = os.path.join(self.tmp, "fw-app.bin")
        with open(self.firmware, "wb") as f:
            f.write(b"x" * 1234)

    def args(self, **kw):
        base = dict(port=None, out=os.path.join(self.tmp, "backup.bin"), firmware=self.firmware, yes=True, backup=False,
                    no_backup=True)
        base.update(kw)
        return argparse.Namespace(**base)

    def run_quiet(self, fn, *a):
        with mock.patch("builtins.print"):
            return fn(*a)

    # check
    def test_check_restarts_the_app_afterwards(self):
        self.assertEqual(self.run_quiet(flash.cmd_check, self.args()), 0)
        self.assertEqual(len(self.fake.resets()), 1)
        self.assertEqual(self.fake.subs()[-1], "read-mac")
        self.assertEqual(self.fake.writes(), [])

    def test_read_only_steps_do_not_reset_the_chip_themselves(self):
        self.run_quiet(flash.cmd_check, self.args())
        for cmd in self.fake.calls[:-1]:
            self.assertIn("no-reset", cmd)

    def test_check_with_a_foreign_partition_table_still_restarts_the_app(self):
        self.fake.ptable = b"\x00" * flash.PTABLE_LEN
        self.assertEqual(self.run_quiet(flash.cmd_check, self.args()), 1)
        self.assertEqual(len(self.fake.resets()), 1)

    def test_check_stopping_on_the_wrong_chip_restarts_the_app(self):
        self.fake.flash_id = "Chip type: ESP32-C3\nDetected flash size: 16MB\n"
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_check, self.args())
        self.assertEqual(len(self.fake.resets()), 1)

    def test_check_stopping_on_the_wrong_flash_size_restarts_the_app(self):
        self.fake.flash_id = "Chip type: ESP32-S3\nDetected flash size: 8MB\n"
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_check, self.args())
        self.assertEqual(len(self.fake.resets()), 1)

    def test_unreadable_partition_table_restarts_the_app(self):
        self.fake.rc["read-flash"] = 2
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_check, self.args())
        self.assertEqual(len(self.fake.resets()), 1)

    def test_a_chip_that_never_answered_is_not_retried(self):
        self.fake.rc["flash-id"] = 2
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_check, self.args())
        self.assertEqual(self.fake.subs(), ["flash-id"])  # no second connect attempt just to reset it

    # backup
    def test_backup_restarts_the_app_afterwards(self):
        self.assertEqual(self.run_quiet(flash.cmd_backup, self.args()), 0)
        self.assertEqual(len(self.fake.resets()), 1)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "backup.bin")))

    def test_failed_backup_restarts_the_app_and_removes_the_partial_file(self):
        out = os.path.join(self.tmp, "backup.bin")
        real = self.fake.__call__

        def failing_backup(cmd, **kw):
            if "read-flash" in cmd and cmd[-1] == out:
                open(out, "wb").close()
                self.fake.calls.append(cmd)
                return subprocess.CompletedProcess(cmd, 2, "", "")
            return real(cmd, **kw)

        with mock.patch.object(flash.subprocess, "run", failing_backup):
            with self.assertRaises(SystemExit):
                self.run_quiet(flash.cmd_backup, self.args())
        self.assertFalse(os.path.exists(out))
        self.assertEqual(len(self.fake.resets()), 1)

    # flash
    def test_refused_flash_restarts_the_app_and_writes_nothing(self):
        self.fake.ptable = b"\x00" * flash.PTABLE_LEN
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_flash, self.args())
        self.assertEqual(self.fake.writes(), [])
        self.assertEqual(len(self.fake.resets()), 1)

    def test_flash_stopping_in_preflight_restarts_the_app(self):
        self.fake.flash_id = "Chip type: ESP32-C3\nDetected flash size: 16MB\n"
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_flash, self.args())
        self.assertEqual(self.fake.writes(), [])
        self.assertEqual(len(self.fake.resets()), 1)

    def test_declined_flash_restarts_the_app(self):
        with mock.patch.object(flash, "ask", lambda *a, **k: False):
            self.assertEqual(self.run_quiet(flash.cmd_flash, self.args(yes=False)), 1)
        self.assertEqual(self.fake.writes(), [])
        self.assertEqual(len(self.fake.resets()), 1)

    def test_successful_flash_writes_only_the_app_slot_with_the_watchdog_reset_and_nothing_after(self):
        self.assertEqual(self.run_quiet(flash.cmd_flash, self.args()), 0)
        writes = self.fake.writes()
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][-2:], [hex(flash.APP_OFFSET), self.firmware])
        self.assertIn("watchdog-reset", writes[0])
        self.assertEqual(self.fake.subs()[-1], "write-flash")  # no extra reset after the write
        self.assertEqual(self.fake.resets(), [])

    def test_failed_write_does_not_pretend_to_restart_the_app(self):
        self.fake.rc["write-flash"] = 2
        with self.assertRaises(SystemExit):
            self.run_quiet(flash.cmd_flash, self.args())
        self.assertEqual(self.fake.resets(), [])  # a half-written slot must stay in the bootloader for the retry

    def test_nothing_but_the_app_slot_is_ever_written(self):
        self.run_quiet(flash.cmd_flash, self.args())
        for cmd in self.fake.writes():
            self.assertEqual(cmd[cmd.index("write-flash") + 1], hex(0x10000))


if __name__ == "__main__":
    unittest.main()
