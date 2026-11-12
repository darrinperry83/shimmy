#ifndef RD_OUTPUT2_POLICY_H
#define RD_OUTPUT2_POLICY_H

// Host-present and the matching outer wl_shm commit must be adjacent. A stale
// marker must never suppress a later CPU fallback after a stalled guest.
static inline int rd_output2_fresh_commit(int pending, double age_seconds) {
  return pending && age_seconds >= 0.0 && age_seconds < 0.1;
}

// A popup may need the ordinary SHM owner composite. Keep that path until the
// popup has its own direct-surface integration.
static inline int rd_output2_direct_allowed(int owner_popup_active) {
  return !owner_popup_active;
}

#endif
