#pragma once
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Host-only session marker. Require the exact private launcher layout and
// ownership on both the 0700 directory and its 0600 regular marker. lstat
// rejects symlink substitution, while nlink==1 rejects hard-linked markers.
static int rd_multiwindow_gate_file_ready(const char* path) {
  static const char prefix[] = "/tmp/rd-hypr-session-";
  static const char suffix[] = "/multiwindow-ready";
  if (!path || strncmp(path, prefix, sizeof(prefix)-1)) return 0;
  const char* slash = strrchr(path, '/');
  if (!slash || strcmp(slash, suffix) || slash <= path + sizeof(prefix)-1)
    return 0;
  for (const char* c = path + sizeof(prefix)-1; c < slash; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
          (*c >= '0' && *c <= '9') || *c == '_'))
      return 0;
  char parent[256];
  size_t len = (size_t)(slash-path);
  if (len >= sizeof(parent)) return 0;
  memcpy(parent,path,len); parent[len]=0;
  struct stat dir, file;
  uid_t uid = geteuid();
  if (lstat(parent,&dir) || !S_ISDIR(dir.st_mode) || dir.st_uid != uid ||
      (dir.st_mode & 077) != 0) return 0;
  if (lstat(path,&file) || !S_ISREG(file.st_mode) || file.st_uid != uid ||
      (file.st_mode & 077) != 0 || file.st_nlink != 1) return 0;
  return 1;
}

// The launcher writes a 16-character lowercase hex nonce and newline. Read
// the opened inode after metadata validation so the ACK binds to that exact
// session marker, even if a pathname is exchanged between lstat and open.
static int rd_multiwindow_gate_read_nonce(const char* marker, char nonce[17]) {
  int fd = open(marker, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return 0;
  struct stat st;
  char bytes[18] = {0};
  ssize_t n = read(fd, bytes, sizeof bytes);
  int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      (st.st_mode & 077) == 0 && st.st_nlink == 1 && n == 17 && bytes[16] == '\n';
  close(fd);
  for (int i = 0; valid && i < 16; i++)
    valid = (bytes[i] >= '0' && bytes[i] <= '9') ||
            (bytes[i] >= 'a' && bytes[i] <= 'f');
  if (valid) memcpy(nonce, bytes, 17);
  return valid;
}
