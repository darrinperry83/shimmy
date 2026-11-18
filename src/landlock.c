// landlock.c — Linux Landlock filesystem sandbox, enforced honestly.
//
// The three syscalls (create_ruleset / add_rule / restrict_self) build a set of
// path-beneath rules. landlock_restrict_self() commits a ruleset into this
// process's active Landlock "domain", and every access-granting / mutating
// filesystem syscall then consults landlock_check() against the guest path it
// is about to touch. A handled right that no committed ruleset grants on that
// path is refused with -EACCES. Nothing here fabricates success.
//
// Process lifetime:
//   * fork() duplicates the host process, so the domain (a process-global) is
//     inherited by the child automatically.
//   * clone(CLONE_VM) runs the child as a thread in this address space, so the
//     same global is shared — again correct.
//   * execve() re-execs the shim binary (a fresh address space), so the domain
//     is serialized to an anonymous fd and carried through an RDLANDLOCK_FD=<n>
//     host env var, exactly like the existing seccomp state (RDSECCOMP_FD).
//
// Stacking: Linux intersects stacked rulesets (most restrictive wins). We model
// that intersection directly in landlock_check(): for each committed ruleset,
// every requested right that ruleset handles must be granted by at least one of
// its rules on the path. A single ruleset is the common case; multiple rulesets
// are merged conservatively, never widened.
//
// Known limitation (documented, not silent): a ruleset fd that the guest dup()s
// is tracked under its original number only, and dup'd ruleset fds are not
// registered. pacman opens one ruleset, adds its rules, restricts, and closes it
// without dup; nothing in the download path relies on dup'd ruleset fds.
#include "landlock.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LL_PATH_MAX 4096          // match GPATH_MAX in syscalls.c
#define LL_MAX_RULESETS 64        // concurrently open (uncommitted) rulesets
#define LL_MAX_RULES 128          // path-beneath rules per ruleset
#define LL_MAX_DOMAIN 16          // stacked committed rulesets

struct ll_rule {
  uint64_t allowed_access;
  char parent[LL_PATH_MAX];       // canonical guest-absolute directory path
};

struct ll_ruleset {
  int fd;                          // host fd that represents this ruleset, or -1
  uint64_t handled_access_fs;
  unsigned nrules;
  struct ll_rule rules[LL_MAX_RULES];
};

static struct ll_ruleset g_rulesets[LL_MAX_RULESETS];

// The committed domain is a deep copy, so closing the originating ruleset fd
// cannot disturb an active restriction.
static struct ll_ruleset g_domain[LL_MAX_DOMAIN];
static unsigned g_domain_n;

// g_rulesets is zero-initialized, but fd 0 is a VALID descriptor number, so a
// zeroed slot would read as "in use by fd 0". Mark every slot free first.
static void ll_init(void) {
  static int done;
  if (done) return;
  done = 1;
  for (int i = 0; i < LL_MAX_RULESETS; i++) g_rulesets[i].fd = -1;
  for (int i = 0; i < LL_MAX_DOMAIN; i++) g_domain[i].fd = -1;
}

static struct ll_ruleset *find_ruleset(int fd) {
  ll_init();
  for (int i = 0; i < LL_MAX_RULESETS; i++)
    if (g_rulesets[i].fd == fd && fd >= 0) return &g_rulesets[i];
  return NULL;
}

static struct ll_ruleset *alloc_slot(void) {
  ll_init();
  for (int i = 0; i < LL_MAX_RULESETS; i++)
    if (g_rulesets[i].fd < 0) return &g_rulesets[i];
  return NULL;
}

// A component-wise prefix test on canonical guest-absolute paths. "/" matches
// everything; otherwise dir must end exactly at a '/' boundary of path.
static int path_is_under(const char *path, const char *dir) {
  size_t dl = strlen(dir);
  if (dl == 1 && dir[0] == '/') return 1;
  if (strncmp(path, dir, dl) != 0) return 0;
  return path[dl] == '/' || path[dl] == 0;
}

long landlock_create_ruleset(uint64_t handled_access_fs, int *out_fd) {
  if (!out_fd) return -22;                       // -EINVAL
  // Unknown bits in handled_access_fs are rejected, as Linux rejects them.
  if (handled_access_fs & ~LL_FS_ALL) return -22; // -EINVAL
  struct ll_ruleset *rs = alloc_slot();
  if (!rs) return -24;                           // -EMFILE
  int fds[2];
  if (pipe(fds) != 0) return -errno;   // ENOMEM/EMFILE/ENFILE: identical macOS<->Linux
  // The read end is an opaque host fd. It must be a real descriptor so the
  // guest's close()/dup()/fcntl() behave, but it carries no data semantics.
  int fd = fds[0];
  close(fds[1]);
  memset(rs, 0, sizeof *rs);
  rs->fd = fd;
  rs->handled_access_fs = handled_access_fs;
  rs->nrules = 0;
  *out_fd = fd;
  return 0;
}

long landlock_add_rule(int ruleset_fd, int rule_type, uint64_t allowed_access,
                       const char *parent_path) {
  struct ll_ruleset *rs = find_ruleset(ruleset_fd);
  if (!rs) return -9;                            // -EBADF
  if (rule_type == LL_RULE_NET_PORT) return -95; // -EOPNOTSUPP (pacman never uses it)
  if (rule_type != LL_RULE_PATH_BENEATH) return -22; // -EINVAL
  if (!parent_path || !parent_path[0]) return -22;
  if (allowed_access & ~LL_FS_ALL) return -22;
  // Linux silently drops access bits the ruleset does not handle, but keeps the
  // rule. We record the full mask; landlock_check() only consults handled bits.
  if (rs->nrules >= LL_MAX_RULES) return -12;    // -ENOMEM (no room)
  size_t n = strlen(parent_path);
  if (n == 0 || n >= LL_PATH_MAX) return -36;    // -ENAMETOOLONG
  struct ll_rule *r = &rs->rules[rs->nrules++];
  r->allowed_access = allowed_access;
  memcpy(r->parent, parent_path, n + 1);
  return 0;
}

long landlock_restrict_self(int ruleset_fd) {
  struct ll_ruleset *rs = find_ruleset(ruleset_fd);
  if (!rs) return -9;                            // -EBADF
  if (rs->nrules == 0 && (rs->handled_access_fs & LL_FS_ALL) == 0)
    return 0;                                    // empty ruleset: no restriction
  if (g_domain_n >= LL_MAX_DOMAIN) return -24;   // -EMFILE (stacking exhausted)
  struct ll_ruleset *dst = &g_domain[g_domain_n++];
  memset(dst, 0, sizeof *dst);
  dst->fd = -1;
  dst->handled_access_fs = rs->handled_access_fs;
  dst->nrules = rs->nrules;
  memcpy(dst->rules, rs->rules, rs->nrules * sizeof dst->rules[0]);
  return 0;
}

long landlock_check(const char *path, uint64_t access) {
  if (!access || g_domain_n == 0) return 0;
  if (!path || !path[0]) return 0;
  for (unsigned i = 0; i < g_domain_n; i++) {
    const struct ll_ruleset *rs = &g_domain[i];
    uint64_t handled = rs->handled_access_fs & access;
    if (!handled) continue;                      // this layer does not restrict these rights
    uint64_t allowed = 0;
    for (unsigned j = 0; j < rs->nrules; j++)
      if (path_is_under(path, rs->rules[j].parent))
        allowed |= rs->rules[j].allowed_access;
    if (handled & ~allowed) return -13;          // -EACCES
  }
  return 0;
}

// Both paths are canonical and start with '/'. Compare their parent directories.
static int same_parent_dir(const char *a, const char *b) {
  const char *pa = strrchr(a, '/');
  const char *pb = strrchr(b, '/');
  if (!pa || !pb) return 0;
  size_t la = (size_t)(pa - a);
  size_t lb = (size_t)(pb - b);
  if (la != lb) return 0;
  return la == 0 || memcmp(a, b, la) == 0;
}

long landlock_check_rename(const char *old_path, const char *new_path,
                           int old_is_dir, int new_exists, int new_is_dir) {
  if (g_domain_n == 0) return 0;
  uint64_t old_remove = old_is_dir ? LL_FS_REMOVE_DIR : LL_FS_REMOVE_FILE;
  uint64_t old_make   = old_is_dir ? LL_FS_MAKE_DIR   : LL_FS_MAKE_REG;
  uint64_t new_remove = new_exists ? (new_is_dir ? LL_FS_REMOVE_DIR
                                                 : LL_FS_REMOVE_FILE)
                                   : 0;

  if (same_parent_dir(old_path, new_path)) {
    // No reparenting: Linux does not require REFER for a same-directory rename.
    return landlock_check(old_path, old_remove | old_make | new_remove);
  }
  long r = landlock_check(old_path, LL_FS_REFER | old_remove);
  if (r < 0) return r;
  return landlock_check(new_path, LL_FS_REFER | old_make | new_remove);
}

int landlock_ruleset_fd(int fd) { return find_ruleset(fd) != NULL; }

void landlock_fd_closed(int fd) {
  struct ll_ruleset *rs = find_ruleset(fd);
  if (rs) { rs->fd = -1; rs->nrules = 0; }
}

int landlock_domain_active(void) { return g_domain_n != 0; }

// ---------------------------------------------------------------------------
// Exec carry: serialize the committed domain into an anonymous fd (a pipe).
// ---------------------------------------------------------------------------
#define LL_WIRE_MAGIC 0x52444c4bu           // "RDLK"
#define LL_WIRE_VERSION 1u

static int write_all(int fd, const void *data, size_t size) {
  const unsigned char *p = data;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n <= 0) return -1;
    p += n; size -= (size_t)n;
  }
  return 0;
}

static int read_all(int fd, void *data, size_t size) {
  unsigned char *p = data;
  while (size) {
    ssize_t n = read(fd, p, size);
    if (n <= 0) return -1;
    p += n; size -= (size_t)n;
  }
  return 0;
}

int landlock_domain_export_fd(void) {
  if (g_domain_n == 0) return -1;
  int fds[2];
  if (pipe(fds) != 0) return -1;
  if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
    close(fds[0]); close(fds[1]); return -1;
  }
  uint32_t total_rules = 0;
  for (unsigned i = 0; i < g_domain_n; i++) total_rules += g_domain[i].nrules;
  uint32_t hdr[4] = { LL_WIRE_MAGIC, LL_WIRE_VERSION, g_domain_n, total_rules };
  int ok = write_all(fds[1], hdr, sizeof hdr) == 0;
  for (unsigned i = 0; ok && i < g_domain_n; i++) {
    const struct ll_ruleset *rs = &g_domain[i];
    uint64_t handled = rs->handled_access_fs;
    uint32_t nrules = rs->nrules;
    ok = write_all(fds[1], &handled, sizeof handled) == 0 &&
         write_all(fds[1], &nrules, sizeof nrules) == 0;
    for (unsigned j = 0; ok && j < nrules; j++) {
      const struct ll_rule *r = &rs->rules[j];
      uint64_t allowed = r->allowed_access;
      uint32_t plen = (uint32_t)strlen(r->parent);
      ok = write_all(fds[1], &allowed, sizeof allowed) == 0 &&
           write_all(fds[1], &plen, sizeof plen) == 0 &&
           write_all(fds[1], r->parent, plen) == 0;
    }
  }
  close(fds[1]);
  if (!ok) { close(fds[0]); return -1; }
  return fds[0];
}

long landlock_domain_import_fd(int fd) {
  struct stat st;
  int flags = fcntl(fd, F_GETFL);
  if (fd < 0 || flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
      fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) return -9; // -EBADF
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -9;
  uint32_t hdr[4];
  if (read_all(fd, hdr, sizeof hdr) != 0 ||
      hdr[0] != LL_WIRE_MAGIC || hdr[1] != LL_WIRE_VERSION ||
      hdr[2] > LL_MAX_DOMAIN || hdr[3] > LL_MAX_DOMAIN * LL_MAX_RULES)
    return -22;                                  // -EINVAL: forged / damaged
  struct ll_ruleset domain[LL_MAX_DOMAIN];
  memset(domain, 0, sizeof domain);
  unsigned total = 0;
  for (unsigned i = 0; i < hdr[2]; i++) {
    struct ll_ruleset *rs = &domain[i];
    uint64_t handled; uint32_t nrules;
    if (read_all(fd, &handled, sizeof handled) != 0 ||
        read_all(fd, &nrules, sizeof nrules) != 0 ||
        nrules > LL_MAX_RULES || (handled & ~LL_FS_ALL)) return -22;
    rs->handled_access_fs = handled;
    rs->nrules = nrules;
    total += nrules;
    for (unsigned j = 0; j < nrules; j++) {
      uint64_t allowed; uint32_t plen;
      if (read_all(fd, &allowed, sizeof allowed) != 0 ||
          read_all(fd, &plen, sizeof plen) != 0 ||
          plen == 0 || plen >= LL_PATH_MAX || (allowed & ~LL_FS_ALL)) return -22;
      if (read_all(fd, rs->rules[j].parent, plen) != 0) return -22;
      rs->rules[j].parent[plen] = 0;
      if (rs->rules[j].parent[0] != '/') return -22;
      rs->rules[j].allowed_access = allowed;
    }
  }
  if (total != hdr[3]) return -22;
  unsigned char extra;
  if (read(fd, &extra, 1) != 0) return -22;      // no trailing garbage
  memcpy(g_domain, domain, sizeof g_domain);
  g_domain_n = hdr[2];
  return 0;
}
