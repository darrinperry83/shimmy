#ifndef SHIMMY_GUEST_KEY_SUBSCRIPTION_H
#define SHIMMY_GUEST_KEY_SUBSCRIPTION_H
#include "guest_key_shared.h"
#include <stdint.h>

/* Internal lifecycle model. It is not wired to shim fork/exec or guest ABI. */
typedef struct gks_subscription gks_subscription;
enum gks_scope { GKS_THREAD_RING, GKS_PROCESS_RING, GKS_SESSION_RING };
gks_subscription *gks_subscription_new(gks_namespace *ns, uint32_t guest_uid);
void gks_subscription_free(gks_subscription *sub);
long gks_subscription_set(gks_subscription *sub, enum gks_scope scope, int32_t ring);
long gks_subscription_search_user(gks_subscription *sub, const char *description);
/* Linux clears thread and process rings on fork/exec; session survives. */
gks_subscription *gks_subscription_fork_child(const gks_subscription *parent);
void gks_subscription_after_exec(gks_subscription *sub);
int32_t gks_subscription_session_serial(const gks_subscription *sub);
#endif
