// Run only in a disposable Shimmy guest root. Native Linux would change UID.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  if (!getenv("SHIMMY_DISPOSABLE_OWNERSHIP_TEST")) return 77;
  const char *runtime = "/run/user/22001";
  const char *child = "/run/user/22001/dbus-1";
  if (mkdir(runtime, 0700) < 0 && errno != EEXIST) return 1;
  if (chown(runtime, 22001, 22001) < 0) return 2;
  struct stat st;
  if (stat(runtime, &st) < 0 || st.st_uid != 22001 || st.st_gid != 22001 ||
      (st.st_mode & 0777) != 0700) return 3;
  if (setgid(22001) < 0 || setuid(22001) < 0) return 4;
  if (mkdir(child, 0700) < 0) { perror("mkdir dbus-1"); return 5; }
  if (stat(child, &st) < 0) return 6;
  printf("runtime child uid=%u gid=%u mode=%03o\n", st.st_uid, st.st_gid,
         (unsigned)st.st_mode & 0777);
  return st.st_uid == 22001 && st.st_gid == 22001 &&
         (st.st_mode & 0777) == 0700 ? 0 : 7;
}
