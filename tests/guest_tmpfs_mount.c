// Run ONLY through Shimmy against a disposable guest root. Native Linux would
// perform real mounts. SHIMMY_DISPOSABLE_MOUNT_TEST is a required second guard.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int expect_mount(const char *target, const char *fstype, int expected) {
  errno = 0;
  int r = mount(fstype, target, fstype, 0, NULL);
  if ((expected == 0 && r == 0) || (expected != 0 && r == -1 && errno == expected))
    return 0;
  fprintf(stderr, "mount(%s,%s): result=%d errno=%d, wanted errno=%d\n",
          target, fstype, r, errno, expected);
  return 1;
}

int main(void) {
  if (!getenv("SHIMMY_DISPOSABLE_MOUNT_TEST")) {
    fputs("refusing mount test without disposable Shimmy marker\n", stderr);
    return 77;
  }
  if (expect_mount("/run/user/22001", "tmpfs", EPERM)) return 1;
  if (expect_mount("/run/user/22001/child", "tmpfs", 0)) return 1;
  if (expect_mount("/run/user/not-a-uid", "tmpfs", 0)) return 1;
  if (expect_mount("/tmp", "tmpfs", 0)) return 1;
  if (expect_mount("/dev", "tmpfs", 0)) return 1;
  if (expect_mount("/run", "tmpfs", 0)) return 1;
  if (expect_mount("/dev/shm", "tmpfs", 0)) return 1;
  if (expect_mount("/dev/pts", "tmpfs", 0)) return 1;
  if (expect_mount("/proc", "proc", 0)) return 1;
  // systemd's mount_nofollow passes a proc fd path for both its early API
  // mounts and this per-user mount. Only the runtime directory may fail.
  if (mkdir("/run/user", 0755) < 0 && errno != EEXIST) return 1;
  if (mkdir("/run/user/23099", 0700) < 0 && errno != EEXIST) return 1;
  int runtime_fd = open("/run/user/23099", O_RDONLY | O_DIRECTORY);
  int early_fd = open("/run", O_RDONLY | O_DIRECTORY);
  if (runtime_fd < 0 || early_fd < 0) return 1;
  char target[64];
  snprintf(target, sizeof target, "/proc/self/fd/%d", runtime_fd);
  if (expect_mount(target, "tmpfs", EPERM)) return 1;
  snprintf(target, sizeof target, "/proc/self/fd/%d", early_fd);
  if (expect_mount(target, "tmpfs", 0)) return 1;
  close(runtime_fd);
  close(early_fd);
  // This is the fallback systemd-user-runtime-dir takes when mount returns
  // EPERM: create/chmod/chown the ordinary directory and verify the guest
  // sees the requested ownership, rather than only host uid 501.
  const char *runtime = "/run/user/23099";
  if (chmod(runtime, 0700) < 0 || chown(runtime, 23099, 23099) < 0) return 1;
  struct stat st = {0};
  if (stat(runtime, &st) < 0 || st.st_uid != 23099 || st.st_gid != 23099) {
    fprintf(stderr, "fallback ownership: uid=%u gid=%u errno=%d\n",
            (unsigned)st.st_uid, (unsigned)st.st_gid, errno);
    return 1;
  }
  puts("tmpfs mount compatibility: pass");
  return 0;
}
