/* ARM64 raw ABI: Landlock filesystem sandbox is enforced, not just acknowledged.
 *
 * Sequence: create a ruleset that handles READ_FILE|WRITE_FILE|READ_DIR|TRUNCATE,
 * add a read-only "/" rule and a read-write rule for /landlock-rw, restrict self,
 * then prove:
 *   - open(O_WRONLY|O_CREAT) inside  /landlock-rw        -> succeeds
 *   - open(O_WRONLY|O_CREAT) outside /landlock-rw ("/")  -> EACCES (-13)
 *   - open(O_RDONLY) of /readable.txt (under "/")         -> succeeds
 */
static long sc(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

#define SYS_mkdirat   34
#define SYS_openat    56
#define SYS_renameat  38
#define SYS_write     64
#define SYS_exit      93
#define SYS_ll_create 444
#define SYS_ll_add    445
#define SYS_ll_restrict 446

#define AT_FDCWD      (-100)
#define O_RDONLY      0
#define O_WRONLY      1
#define O_CREAT       0x40
#define O_TRUNC       0x200
#define O_DIRECTORY   0x4000
#define O_CLOEXEC     0x80000
#define O_PATH        0x200000

#define LL_READ_FILE   (1ULL << 2)
#define LL_WRITE_FILE  (1ULL << 1)
#define LL_READ_DIR    (1ULL << 3)
#define LL_REMOVE_FILE (1ULL << 5)
#define LL_MAKE_REG    (1ULL << 8)
#define LL_TRUNCATE    (1ULL << 14)
#define LL_RULE_PATH_BENEATH 1
#define LL_CREATE_VERSION 1

struct ruleset_attr { unsigned long long handled_access_fs, handled_access_net; };
struct path_beneath_attr { unsigned long long allowed_access; int parent_fd; unsigned reserved; };

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LANDLOCK_GUEST_OK\n";
  if (!code) sc(SYS_write, 1, (long)ok, sizeof ok - 1, 0);
  sc(SYS_exit, code, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  /* Version query: attr == NULL, flags == LANDLOCK_CREATE_RULESET_VERSION. */
  if (sc(SYS_ll_create, 0, 0, LL_CREATE_VERSION, 0) != 3) done(1);

  /* A file under "/" that the read-only "/" rule must still allow. */
  {
    long fd = sc(SYS_openat, AT_FDCWD, (long)"/readable.txt",
                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) done(2);
    if (sc(57, fd, 0, 0, 0) != 0) done(3);
  }
  if (sc(SYS_mkdirat, AT_FDCWD, (long)"/landlock-rw", 0755, 0) != 0) done(4);

  long root_fd = sc(SYS_openat, AT_FDCWD, (long)"/",
                    O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
  if (root_fd < 0) done(5);
  long rw_fd = sc(SYS_openat, AT_FDCWD, (long)"/landlock-rw",
                  O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
  if (rw_fd < 0) done(6);

  struct ruleset_attr attr = {0};
  attr.handled_access_fs = LL_READ_FILE | LL_WRITE_FILE | LL_READ_DIR |
                           LL_REMOVE_FILE | LL_MAKE_REG | LL_TRUNCATE;
  attr.handled_access_net = 0;
  long ruleset_fd = sc(SYS_ll_create, (long)&attr, sizeof attr, 0, 0);
  if (ruleset_fd < 0) done(7);

  struct path_beneath_attr root_rule = {0};
  root_rule.allowed_access = LL_READ_FILE | LL_READ_DIR;
  root_rule.parent_fd = (int)root_fd;
  if (sc(SYS_ll_add, ruleset_fd, LL_RULE_PATH_BENEATH, (long)&root_rule, 0) != 0)
    done(8);

  struct path_beneath_attr rw_rule = {0};
  rw_rule.allowed_access = LL_READ_FILE | LL_WRITE_FILE | LL_READ_DIR |
                           LL_REMOVE_FILE | LL_MAKE_REG | LL_TRUNCATE;
  rw_rule.parent_fd = (int)rw_fd;
  if (sc(SYS_ll_add, ruleset_fd, LL_RULE_PATH_BENEATH, (long)&rw_rule, 0) != 0)
    done(9);

  if (sc(SYS_ll_restrict, ruleset_fd, 0, 0, 0) != 0) done(10);

  /* 1. Write inside the allowed directory must succeed. */
  long inside = sc(SYS_openat, AT_FDCWD, (long)"/landlock-rw/inside.txt",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (inside < 0) done(11);
  if (sc(57, inside, 0, 0, 0) != 0) done(12);

  /* 2. Write outside the allowed directory must fail with EACCES. */
  long outside = sc(SYS_openat, AT_FDCWD, (long)"/landlock-denied.txt",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outside != -13) done(13);          /* not exactly EACCES: sandbox is not enforced */

  /* 3. Read of a file under "/" must still succeed via the "/" rule. */
  long readable = sc(SYS_openat, AT_FDCWD, (long)"/readable.txt", O_RDONLY, 0);
  if (readable < 0) done(14);
  if (sc(57, readable, 0, 0, 0) != 0) done(15);

  /* 4. Same-directory rename needs no REFER (Linux current_check_refer_path):
   *    REMOVE_FILE on the source + MAKE_REG of the source type are enough. */
  if (sc(SYS_renameat, AT_FDCWD, (long)"/landlock-rw/inside.txt",
         AT_FDCWD, (long)"/landlock-rw/renamed.txt") != 0)
    done(16);

  done(0);
}
