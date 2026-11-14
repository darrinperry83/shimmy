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
//   shimmy — LAUNCH the windowserver stand-in (shim_windowserver) as a CHILD with
//            RDNATIVE_WINDOWSERVER_SYSTEM=1. The stand-in — not the chooser — performs the
//            real check-in (bootstrap_check_in "com.apple.windowserver" then
//            "com.apple.windowserver.active", lowercase, that order, in the system bootstrap
//            domain) and runs the ~37-function CGS/SLS server. The child inherits the
//            chooser's task bootstrap port (the launchd-pre-created names), so the check-in
//            resolves exactly as if the chooser itself did it. The chooser (parent) stays
//            alive as the launchd job's primary process and WAITS. On a clean stand-in exit
//            — or a "macOS" key on fd 0, which SIGTERMs the stand-in — the chooser
//            WARM-SWITCHES: the stand-in's exit releases the receive rights it checked in
//            (the names go dead), and the chooser relaunches the real WindowServer
//            (branch_macos). Any crash also falls back to macOS.
//
// SAFETY:
//   * macOS is always the default — a timeout, EOF, or any error boots macOS.
//   * --dry-run prints the full plan and touches NOTHING (never forks, never kickstarts).
//   * SHIMMY_WINDOWSERVER_SYSTEM=0 runs the stand-in in its *test-name* mode
//     (bootstrap_register com.shimmy.windowserver.test), so the launch + server +
//     warm-switch handback can be proven headlessly WITHOUT the real com.apple.windowserver
//     or the system domain.
//   * The actual `launchctl kickstart` of the real WindowServer is OPT-IN
//     (SHIMMY_WARM_SWITCH_KICKSTART=1). Without it the warm-switch prints the command and
//     exits. Running the binary by hand therefore cannot restart the live WindowServer,
//     and even in system mode the stand-in refuses to check in the real name unless root
//     under the launchd job that owns it.
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
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SVC_MAIN   "com.apple.windowserver"
#define SVC_ACTIVE "com.apple.windowserver.active"
#define TIMEOUT_DEFAULT 10
#define SHIMMY_WINDOWSERVER_DEFAULT_PATH "/usr/local/sbin/shim_windowserver"

typedef enum { BRANCH_MACOS = 0, BRANCH_SHIMMY = 1 } branch_t;

static const char *branch_name(branch_t b) {
  return b == BRANCH_SHIMMY ? "shimmy" : "macOS";
}

static void usage(FILE *f) {
  fprintf(f,
    "usage: chooser [--dry-run] [--choice macOS|shimmy] [--kickstart-windowserver]\n"
    "               [--checkin <name> [seconds]]\n"
    "  --dry-run               print the chosen branch + full plan, touch nothing\n"
    "  --choice <branch>       force a branch (headless testing); macOS is the fallback\n"
    "  --kickstart-windowserver  on the macOS branch, kickstart system/com.apple.WindowServer\n"
    "  --checkin <name> [s]    test hook: bootstrap_check_in ONE name, hold it `s` seconds\n"
    "env: SHIMMY_BOOT_TIMEOUT (default 10), SHIMMY_BOOT_CHOICE\n"
    "     SHIMMY_WINDOWSERVER_CMD     stand-in path (default %s)\n"
    "     SHIMMY_WINDOWSERVER_SYSTEM  1 (default) real check-in; 0 = test names (safe)\n"
    "     SHIMMY_WARM_SWITCH_KICKSTART 0 (default) print only; 1 = really kickstart\n",
    SHIMMY_WINDOWSERVER_DEFAULT_PATH);
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

// Poll fd 0 for a key, in milliseconds; returns the byte, or -1 on timeout/EOF.
static int poll_key(int timeout_ms) {
  struct pollfd pfd;
  pfd.fd = 0;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int r = poll(&pfd, 1, timeout_ms);
  if (r <= 0) return -1;
  unsigned char c = 0;
  ssize_t n = read(0, &c, 1);
  if (n != 1) return -1;
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

// The warm-switch key is an EXPLICIT "back to macOS" key only. '\n'/'\r' are deliberately
// excluded: the menu read above consumes one byte, so a leftover newline from "2<enter>" would
// otherwise be misread as an immediate warm-switch.
static int is_warm_switch_key(int key) {
  switch (key) {
    case '1': case 'm': case 'M': case '0': return 1;
    default:                                return 0;
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

// "1" means the flag is on; any other value (or unset) means off.
static int env_flag(const char *name) {
  const char *v = getenv(name);
  return v && strcmp(v, "1") == 0;
}

static const char *windowserver_cmd(void) {
  const char *v = getenv("SHIMMY_WINDOWSERVER_CMD");
  return (v && *v) ? v : SHIMMY_WINDOWSERVER_DEFAULT_PATH;
}

// Default: system mode (real check-in). SHIMMY_WINDOWSERVER_SYSTEM=0 selects the stand-in's
// safe test-name mode for headless validation.
static int windowserver_system_mode(void) {
  const char *v = getenv("SHIMMY_WINDOWSERVER_SYSTEM");
  return !(v && strcmp(v, "0") == 0);
}

static int warm_switch_kickstart_enabled(void) {
  return env_flag("SHIMMY_WARM_SWITCH_KICKSTART");
}

static int branch_macos(int kickstart, int dry_run) {
  printf("chooser: branch = macOS (normal boot)\n");
  if (kickstart) {
    if (dry_run) {
      printf("chooser: dry-run: would kickstart system/com.apple.WindowServer\n");
    } else {
      printf("chooser: kickstarting system/com.apple.WindowServer\n");
      int rc = system("/bin/launchctl kickstart -k system/com.apple.WindowServer");
      if (rc != 0) {
        fprintf(stderr, "chooser: kickstart returned %d (ignored)\n", rc);
      }
    }
  }
  // Normal path: exit 0; loginwindow/launchd start the real WindowServer on demand.
  return 0;
}

// Reap `pid` after a SIGTERM, escalating to SIGKILL if it does not exit within 5 s. The
// stand-in installs a SIGTERM handler that exits cleanly; this is the fail-safe watchdog.
static void wait_with_escalation(pid_t pid, int *status) {
  int waited_ms = 0;
  for (;;) {
    pid_t r = waitpid(pid, status, WNOHANG);
    if (r == pid) return;
    if (r < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (waited_ms >= 5000) {
      fprintf(stderr, "chooser: stand-in did not exit after SIGTERM; sending SIGKILL\n");
      kill(pid, SIGKILL);
      waitpid(pid, status, 0);
      return;
    }
    usleep(100 * 1000);
    waited_ms += 100;
  }
}

static int branch_shimmy(int dry_run) {
  const char *ws = windowserver_cmd();
  int system_mode = windowserver_system_mode();
  int warm_kick = warm_switch_kickstart_enabled();

  printf("chooser: branch = shimmy (takeover)\n");
  printf("chooser: stand-in = %s (%s)\n", ws,
         system_mode ? "RDNATIVE_WINDOWSERVER_SYSTEM=1 -> real check-in"
                     : "test-name mode (safe: no real check-in)");

  if (dry_run) {
    printf("chooser: dry-run: would fork+exec %s with RDNATIVE_WINDOWSERVER_SYSTEM=1 so it\n",
           ws);
    printf("chooser: dry-run:   checks in %s then %s and runs the ~37-function server,\n",
           SVC_MAIN, SVC_ACTIVE);
    printf("chooser: dry-run:   then on clean exit / macOS key (1/m) warm-switch to the real "
           "WindowServer\n");
    printf("chooser: dry-run:   warm-switch kickstart: %s\n",
           warm_kick ? "launchctl kickstart -k system/com.apple.WindowServer"
                     : "disabled (print only)");
    return 0;
  }

  if (access(ws, X_OK) != 0) {
    fprintf(stderr, "chooser: stand-in not executable: %s (%s); fail-safe -> macOS\n",
            ws, strerror(errno));
    return branch_macos(warm_kick, 0);
  }

  pid_t child = fork();
  if (child < 0) {
    perror("chooser: fork");
    return branch_macos(warm_kick, 0);
  }
  if (child == 0) {
    // Child: become the WindowServer stand-in. It performs the real bootstrap check-in and
    // runs the server loop. (Test mode omits the env var, so it registers test names.)
    if (system_mode) setenv("RDNATIVE_WINDOWSERVER_SYSTEM", "1", 1);
    execl(ws, ws, (char *)NULL);
    perror("chooser: execl stand-in");
    _exit(127);
  }

  // Parent: stay alive as the launchd job's primary process; wait for a clean stand-in exit
  // OR a "macOS" key (which asks the stand-in to shut down cleanly).
  printf("chooser: stand-in pid %d running (macOS key 1/m or SIGTERM to warm-switch)\n",
         (int)child);
  fflush(stdout);

  int status = 0;
  int reaped = 0;
  for (;;) {
    pid_t r = waitpid(child, &status, WNOHANG);
    if (r == child) { reaped = 1; break; }
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("chooser: waitpid");
      break;
    }
    int key = poll_key(250);   // 250 ms cadence so we reap promptly
    if (key >= 0 && is_warm_switch_key(key)) {
      printf("chooser: macOS key '%c' -> warm-switch (SIGTERM to stand-in %d)\n",
             key, (int)child);
      fflush(stdout);
      if (kill(child, SIGTERM) != 0 && errno != ESRCH) perror("chooser: kill(SIGTERM)");
      wait_with_escalation(child, &status);
      reaped = 1;
      break;
    }
  }
  if (!reaped) {
    // waitpid errored out (non-EINTR); block-reap to avoid leaving a zombie.
    waitpid(child, &status, 0);
  }

  int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (WIFEXITED(status)) {
    printf("chooser: stand-in exited (status %d)\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("chooser: stand-in terminated by signal %d\n", WTERMSIG(status));
  }
  if (!clean) {
    fprintf(stderr, "chooser: stand-in did not exit cleanly; fail-safe -> macOS\n");
  }

  // The stand-in's exit already released the two receive rights it checked in, so the service
  // names are dead. Warm-switch: relaunch the real WindowServer.
  printf("chooser: warm-switch: handing back to the real WindowServer\n");
  return branch_macos(warm_kick, 0);
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
  }
  if (b == BRANCH_SHIMMY) return branch_shimmy(dry_run);
  return branch_macos(kickstart, dry_run);
}
