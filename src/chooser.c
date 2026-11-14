// chooser.c — the shimmy boot chooser (early launchd daemon, runs BEFORE WindowServer).
//
// Two-option key menu with a 10-second countdown that defaults to macOS:
//
//        shimmy
//    [1] Boot macOS
//    [2] Boot shimmy (Linux)
//    (defaults to macOS in 10 s)
//
// Branches:
//   macOS  — the NORMAL path. The chooser exits 0 and launchd's normal boot continues;
//            loginwindow starts the real WindowServer on demand. Nothing is registered.
//            (An optional --kickstart-windowserver makes the "launch the real WindowServer"
//             step explicit, but it is off by default because the normal path already does it.)
//   shimmy — mirror the real WindowServer's registration EXACTLY:
//                bootstrap_check_in(bp, "com.apple.windowserver",        &main_port)
//                bootstrap_check_in(bp, "com.apple.windowserver.active", &active_port)
//            (lowercase names, in that order, in the system bootstrap domain), then continue
//            into the Linux launch (exec $SHIMMY_LINUX_CMD). The receive rights are
//            pre-created by launchd from THIS job's MachServices dict, so the real
//            WindowServer's own bootstrap_check_in later fails and it exits.
//
// SAFETY:
//   * macOS is always the default — a timeout, EOF, or any error boots macOS.
//   * --dry-run prints the chosen branch and touches nothing.
//   * The real service names are only checked in during a live boot (non-dry-run) shimmy
//     branch. Running the binary by hand cannot touch the live WindowServer: a non-system
//     process's task bootstrap port is its own per-user gui domain, so bootstrap_check_in
//     registers there (harmlessly), never in the system domain where WindowServer lives.
//   * --checkin <name> <seconds> is a test hook: it checks in ONE arbitrary name (used with a
//     scratch launchd job in the caller's own gui domain) so the exact check-in code path can
//     be exercised headlessly without touching com.apple.windowserver or the system domain.
//
// The on-screen menu / direct framebuffer present is a follow-up; this first cut reads a key
// from fd 0 (launchd connects it to StandardInPath).

#include <servers/bootstrap.h>
#include <mach/mach.h>
#include <mach/task_special_ports.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SVC_MAIN   "com.apple.windowserver"
#define SVC_ACTIVE "com.apple.windowserver.active"
#define TIMEOUT_DEFAULT 10

typedef enum { BRANCH_MACOS = 0, BRANCH_SHIMMY = 1 } branch_t;

static const char *branch_name(branch_t b) {
  return b == BRANCH_SHIMMY ? "shimmy" : "macOS";
}

static void usage(FILE *f) {
  fprintf(f,
    "usage: chooser [--dry-run] [--choice macOS|shimmy] [--kickstart-windowserver]\n"
    "               [--checkin <name> [seconds]]\n"
    "  --dry-run               print the chosen branch, touch nothing\n"
    "  --choice <branch>       force a branch (headless testing); macOS is the fallback\n"
    "  --kickstart-windowserver  on macOS branch, kickstart system/com.apple.WindowServer\n"
    "  --checkin <name> [s]    test hook: bootstrap_check_in ONE name, hold it `s` seconds\n"
    "env: SHIMMY_BOOT_TIMEOUT (default 10), SHIMMY_BOOT_CHOICE, SHIMMY_LINUX_CMD\n");
}

static int boot_timeout(void) {
  const char *v = getenv("SHIMMY_BOOT_TIMEOUT");
  if (v && *v) {
    char *end = NULL;
    long t = strtol(v, &end, 10);
    if (end && *end == '\0' && t > 0) return (int)t;
  }
  return TIMEOUT_DEFAULT;
}

// Read one byte from fd 0 with a timeout in seconds; returns the byte, or 0 on timeout/EOF.
static int read_key(int timeout_s) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int r = poll(&pfd, 1, timeout_s * 1000);
  if (r <= 0) return 0;
  unsigned char c = 0;
  ssize_t n = read(0, &c, 1);
  if (n != 1) return 0;
  return (int)c;
}

// Any unrecognised key is macOS — the safe default is the whole point of the chooser.
static branch_t key_to_branch(int key) {
  switch (key) {
    case '2': case 's': case 'S': case 'l': case 'L': return BRANCH_SHIMMY;
    case '1': case 'm': case 'M': case '0': case '\n': case '\r':
    default:                                    return BRANCH_MACOS;
  }
}

static branch_t resolve_branch(const char *forced) {
  if (forced) {
    return strcmp(forced, "shimmy") == 0 ? BRANCH_SHIMMY : BRANCH_MACOS;
  }
  const char *env = getenv("SHIMMY_BOOT_CHOICE");
  if (env && *env) {
    return strcmp(env, "shimmy") == 0 ? BRANCH_SHIMMY : BRANCH_MACOS;
  }
  int t = boot_timeout();
  printf("shimmy\n  [1] Boot macOS\n  [2] Boot shimmy (Linux)\n(defaults to macOS in %d s)\n",
         t);
  fflush(stdout);
  int key = read_key(t);
  if (key == 0) {
    printf("(timeout / no input -> macOS)\n");
  }
  return key_to_branch(key);
}

// The exact registration the real WindowServer performs (see
// WINDOWSERVER_BOOTSTRAP_REGISTRATION.md §5): bootstrap_check_in, lowercase names,
// main service first, then `.active`. NOT bootstrap_register.
static int check_in_windowserver_services(void) {
  mach_port_t bp = MACH_PORT_NULL;
  kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bp);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "chooser: task_get_bootstrap_port failed: %s\n", mach_error_string(kr));
    return -1;
  }

  mach_port_t main_port = MACH_PORT_NULL, active_port = MACH_PORT_NULL;

  kr = bootstrap_check_in(bp, SVC_MAIN, &main_port);
  if (kr != BOOTSTRAP_SUCCESS) {
    fprintf(stderr, "chooser: bootstrap_check_in(%s) failed: %s\n",
            SVC_MAIN, bootstrap_strerror(kr));
    return -1;
  }
  printf("chooser: checked in %s (port 0x%x)\n", SVC_MAIN, main_port);

  kr = bootstrap_check_in(bp, SVC_ACTIVE, &active_port);
  if (kr != BOOTSTRAP_SUCCESS) {
    fprintf(stderr, "chooser: bootstrap_check_in(%s) failed: %s\n",
            SVC_ACTIVE, bootstrap_strerror(kr));
    return -1;
  }
  printf("chooser: checked in %s (port 0x%x)\n", SVC_ACTIVE, active_port);

  return 0;
}

// Test hook: check in ONE arbitrary name and hold it. Used with a scratch launchd job in the
// caller's own gui domain to prove the check-in path headlessly (see the validation report).
static int do_checkin(const char *name, int hold_s) {
  mach_port_t bp = MACH_PORT_NULL;
  kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bp);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "checkin: task_get_bootstrap_port failed: %s\n", mach_error_string(kr));
    return 1;
  }
  mach_port_t port = MACH_PORT_NULL;
  kr = bootstrap_check_in(bp, name, &port);
  printf("checkin: bootstrap_check_in(%s) -> kr=%d (%s) port=0x%x\n",
         name, kr, bootstrap_strerror(kr), port);
  fflush(stdout);
  if (kr != BOOTSTRAP_SUCCESS) return 1;
  printf("checkin: holding %s for %d s (verify with: launchctl print ...)\n", name, hold_s);
  fflush(stdout);
  sleep((unsigned)hold_s);
  return 0;
}

static int branch_macos(int kickstart) {
  printf("chooser: branch = macOS (normal boot)\n");
  if (kickstart) {
    printf("chooser: kickstarting system/com.apple.WindowServer\n");
    int rc = system("/bin/launchctl kickstart -k system/com.apple.WindowServer");
    if (rc != 0) {
      fprintf(stderr, "chooser: kickstart returned %d (ignored)\n", rc);
    }
  }
  // Normal path: exit 0; loginwindow/launchd start the real WindowServer on demand.
  return 0;
}

static int branch_shimmy(int dry_run) {
  printf("chooser: branch = shimmy (takeover)\n");
  if (dry_run) {
    printf("chooser: dry-run: would bootstrap_check_in %s + %s, then exec $SHIMMY_LINUX_CMD\n",
           SVC_MAIN, SVC_ACTIVE);
    return 0;
  }

  if (check_in_windowserver_services() != 0) {
    fprintf(stderr, "chooser: check-in failed; aborting (fail-safe: macOS wins)\n");
    return 1;
  }

  const char *cmd = getenv("SHIMMY_LINUX_CMD");
  if (!cmd || !*cmd) {
    // First cut: no Linux launch command configured. Stay alive owning the service so the
    // real WindowServer's check-in fails and it exits. The Linux launch is wired next.
    printf("chooser: SHIMMY_LINUX_CMD unset; holding the windowserver services\n");
    fflush(stdout);
    for (;;) pause();
  }

  printf("chooser: exec %s\n", cmd);
  fflush(stdout);
  execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
  perror("execl");
  return 1;
}

int main(int argc, char **argv) {
  int dry_run = 0;
  int kickstart = 0;
  const char *forced = NULL;
  const char *checkin_name = NULL;
  int checkin_hold = 30;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--dry-run") || !strcmp(a, "-n")) {
      dry_run = 1;
    } else if (!strcmp(a, "--choice") && i + 1 < argc) {
      forced = argv[++i];
    } else if (!strcmp(a, "--kickstart-windowserver")) {
      kickstart = 1;
    } else if (!strcmp(a, "--checkin") && i + 1 < argc) {
      checkin_name = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') checkin_hold = atoi(argv[++i]);
    } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "chooser: unknown argument: %s\n", a);
      usage(stderr);
      return 2;
    }
  }

  setbuf(stdout, NULL);

  if (checkin_name) {
    return do_checkin(checkin_name, checkin_hold);
  }

  branch_t b = resolve_branch(forced);
  if (dry_run) {
    printf("chooser: dry-run -> %s\n", branch_name(b));
    return 0;
  }
  if (b == BRANCH_SHIMMY) return branch_shimmy(0);
  return branch_macos(kickstart);
}
