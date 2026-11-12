#ifndef RD_PRESENT_FEEDBACK_H
#define RD_PRESENT_FEEDBACK_H
#include <stdint.h>
/* Research-only cross-thread wake gate. The Wayland thread owns callback IDs.
 * The Metal completion thread only publishes a sequence and writes a wake byte. */
#define RD_FEEDBACK_OUTPUTS 2
int rd_feedback_init(void);
int rd_feedback_fd(void);
uint64_t rd_feedback_submit(unsigned output);
uint64_t rd_feedback_submitted(unsigned output);
void rd_feedback_finish(unsigned output, uint64_t sequence);
uint64_t rd_feedback_completed(unsigned output);
void rd_feedback_drain(void);
void rd_feedback_close(void);
#endif
