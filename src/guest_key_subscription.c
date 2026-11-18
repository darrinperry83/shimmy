#include "guest_key_subscription.h"
#include <errno.h>
#include <stdlib.h>

struct gks_subscription {
  gks_namespace *ns;
  uint32_t uid;
  int32_t rings[3];
};

gks_subscription *gks_subscription_new(gks_namespace *ns, uint32_t guest_uid) {
  if (!ns) return NULL;
  gks_subscription *sub = calloc(1, sizeof *sub);
  if (!sub) return NULL;
  sub->ns = ns;
  sub->uid = guest_uid;
  return sub;
}
void gks_subscription_free(gks_subscription *sub) { free(sub); }

long gks_subscription_set(gks_subscription *sub, enum gks_scope scope, int32_t ring) {
  if (!sub || scope < GKS_THREAD_RING || scope > GKS_SESSION_RING || ring < 0)
    return -EINVAL;
  if (ring) {
    long result = gks_ring_check_owner(sub->ns, sub->uid, ring);
    if (result) return result;
  }
  sub->rings[scope] = ring;
  return 0;
}

long gks_subscription_search_user(gks_subscription *sub, const char *description) {
  if (!sub || !description) return -EINVAL;
  for (int scope = GKS_THREAD_RING; scope <= GKS_SESSION_RING; scope++) {
    if (!sub->rings[scope]) continue;
    long result = gks_ring_search_user(sub->ns, sub->uid, sub->rings[scope], description);
    if (result != -ENOENT) return result;
  }
  return -ENOENT;
}

gks_subscription *gks_subscription_fork_child(const gks_subscription *parent) {
  if (!parent) return NULL;
  gks_subscription *child = gks_subscription_new(parent->ns, parent->uid);
  if (child) child->rings[GKS_SESSION_RING] = parent->rings[GKS_SESSION_RING];
  return child;
}
void gks_subscription_after_exec(gks_subscription *sub) {
  if (!sub) return;
  sub->rings[GKS_THREAD_RING] = 0;
  sub->rings[GKS_PROCESS_RING] = 0;
}
int32_t gks_subscription_session_serial(const gks_subscription *sub) {
  return sub ? sub->rings[GKS_SESSION_RING] : 0;
}
