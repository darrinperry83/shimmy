#ifndef RD_UNIX_OUTQ_PEER_H
#define RD_UNIX_OUTQ_PEER_H

// Read the macOS receive-buffer charge of the connected AF_UNIX peer.
// This remains positive while an SCM_RIGHTS message is queued and drops after
// recvmsg consumes it. Returns 0 on success, -1/errno on any uncertainty.
int rd_unix_peer_outq(int fd, int* queued_bytes);

#endif
