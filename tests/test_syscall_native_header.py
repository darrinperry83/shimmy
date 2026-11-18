"""Regression for native AArch64 versus generic compat-only syscall macros."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docs/syscalls"))
from native_header import numbered_native_entries


class NativeHeaderTest(unittest.TestCase):
    def test_compat_guard_and_following_native_entry(self):
        native, compat = numbered_native_entries("""
#define __NR_read 63
#if defined(__SYSCALL_COMPAT) || __BITS_PER_LONG == 32
#define __NR_clock_gettime64 403
#if defined(INNER)
#define __NR_clock_settime64 404
#endif
#define __NR_sched_rr_get_interval_time64 423
#endif
#define __NR_pidfd_send_signal 424
""")
        self.assertEqual(native, {63: "read", 424: "pidfd_send_signal"})
        self.assertEqual(compat, {403: "clock_gettime64", 404: "clock_settime64",
                                  423: "sched_rr_get_interval_time64"})

    def test_unclosed_guard_fails(self):
        with self.assertRaises(ValueError):
            numbered_native_entries("#if defined(__SYSCALL_COMPAT) || __BITS_PER_LONG == 32\n"
                                    "#define __NR_clock_gettime64 403\n")


if __name__ == "__main__":
    unittest.main()
