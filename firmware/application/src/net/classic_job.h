#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded read-only jobs, owned by the current web reader lease.
 * Checks/reads use the batch request with up to 80/40 entries. Capture kind 2:
 * v1, kind, UID length, UID, ordinary 12-byte collect request, total BE16;
 * the request's count selects 1..16 observations per cooperative RF step.
 * Events: v1, job BE32, sequence BE16, flags (done=1,cancelled=2), kind,
 * RF status BE16, completed count BE16, observation/sector/check bytes. */
#define CLASSIC_JOB_EVENT_MAX 285u
bool classic_job_active(void);
bool classic_job_start(const uint8_t *, size_t, bool, uint32_t *);
bool classic_job_cancel(uint32_t);
void classic_job_reset(void);
void classic_job_poll(void);
size_t classic_job_read(uint32_t, uint8_t *, size_t);
