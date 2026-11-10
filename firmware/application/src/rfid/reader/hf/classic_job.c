#include "classic_job.h"
#include "nfca_commands.h"
#include "nfca_session.h"
#include "app_status.h"
#include "data_cmd.h"
#include <string.h>

#define QUEUE_DEPTH 4u
#define REQUEST_MAX (4u + 10u + 80u * 8u)
static uint32_t next_id;
static bool initialized;
/* The ordinary RAM region is already reserved for RF/network workspaces,
 * heap and stack. Use spare upper RAM; initialize it explicitly on every boot
 * and discard all job state when its owning web session ends. */
static struct {
    uint32_t id;
    uint16_t sequence, completed, total;
    uint8_t request[REQUEST_MAX], uid_length, kind, head;
    uint8_t events[QUEUE_DEPTH][CLASSIC_JOB_EVENT_MAX];
    uint16_t sizes[QUEUE_DEPTH];
    uint8_t first, queued;
    bool running, cancel, identified;
} job
#if defined(NRF52840_XXAA)
    __attribute__((section(".noinit.classic_job")))
#endif
;

static void initialize(void) {
    if (!initialized) { memset(&job, 0, sizeof(job)); initialized = true; }
}

bool classic_job_active(void) { initialize(); return job.running || job.queued; }
void classic_job_reset(void) { initialize(); memset(&job, 0, sizeof(job)); nfca_session_close(); }

bool classic_job_start(const uint8_t *p, size_t size, uint32_t *id) {
    initialize();
    if (job.running || job.queued || !p || size < 7 || size > REQUEST_MAX || p[0] != 1 || p[1] > 2 ||
        (p[2] != 4 && p[2] != 7 && p[2] != 10) || next_id == UINT32_MAX) return false;
    unsigned head = 4 + p[2], total;
    if (p[1] == 2) {
        head--;
        if (size != head + 14 || p[head + 10] > 1 || !p[head + 11] || p[head + 11] > 16 ||
            (p[head] != 0x60 && p[head] != 0x61) ||
            (p[head + 8] != 0x60 && p[head + 8] != 0x61)) return false;
        total = p[head + 12] * 256u + p[head + 13];
        if (!total || total > 4096) return false;
    } else {
        if (size < head) return false;
        total = p[head - 1];
        if (!total || total > (p[1] ? 40u : 80u) || size != head + total * 8u) return false;
        for (unsigned i = 0; i < total; i++) {
            const uint8_t *entry = p + head + i * 8u;
            if ((entry[1] != 0x60 && entry[1] != 0x61) || (p[1] && entry[0] >= 40)) return false;
        }
    }
    memset(&job, 0, sizeof(job));
    memcpy(job.request, p, size);
    job.id = ++next_id; *id = job.id;
    job.kind = p[1]; job.uid_length = p[2]; job.head = head;
    job.total = total; job.running = true;
    return true;
}

bool classic_job_cancel(uint32_t id) {
    initialize();
    if (!job.id || id != job.id) return false;
    job.cancel = true; return true;
}

size_t classic_job_read(uint32_t id, uint8_t *output, size_t capacity) {
    initialize();
    if (!id || id != job.id || !job.queued || capacity < job.sizes[job.first]) return 0;
    unsigned size = job.sizes[job.first];
    memcpy(output, job.events[job.first], size);
    job.first = (job.first + 1) % QUEUE_DEPTH; job.queued--;
    return size;
}

static void emit(uint16_t status, const uint8_t *data, unsigned size, uint8_t flags) {
    unsigned slot = (job.first + job.queued) % QUEUE_DEPTH;
    uint8_t *p = job.events[slot];
    p[0] = 1;
    for (unsigned i = 0; i < 4; i++) p[1 + i] = job.id >> (24 - i * 8);
    p[5] = job.sequence >> 8; p[6] = job.sequence++; p[7] = flags; p[8] = job.kind;
    p[9] = status >> 8; p[10] = status;
    p[11] = job.completed >> 8; p[12] = job.completed;
    if (size) memcpy(p + 13, data, size);
    job.sizes[slot] = size + 13; job.queued++;
    if (flags & 1) { job.running = false; nfca_session_close(); }
}

void classic_job_poll(void) {
    initialize();
    if (!job.running || job.queued == QUEUE_DEPTH) return;
    if (job.cancel) { emit(STATUS_HF_TAG_OK, NULL, 0, 3); return; }
    uint8_t output[NFCA_COMMAND_OUTPUT_BYTES];
    uint16_t size = 0, status;
    if (!job.identified) {
        nfca_session_close();
        job.identified = true;
        if (job.kind != 1) nfca_session_open();
        return;
    }
    uint8_t request[13 + 10];
    uint16_t command, request_size, advance = 1;
    if (job.kind == 2) {
        command = DATA_CMD_MF1_COLLECT_OBSERVATIONS; request_size = 13 + job.uid_length;
        memcpy(request, job.request + job.head, 12);
        request[12] = job.uid_length;
        memcpy(request + 13, job.request + 3, job.uid_length);
        if (request[11] > job.total - job.completed) request[11] = job.total - job.completed;
        advance = request[11];
    } else {
        const uint8_t *entry = job.request + job.head + job.completed * 8u;
        if (job.kind) {
            command = DATA_CMD_MF1_READ_SECTOR; request_size = 9 + job.uid_length;
            request[0] = entry[1]; request[1] = entry[0];
            memcpy(request + 2, entry + 2, 6);
            request[8] = job.uid_length; memcpy(request + 9, job.request + 3, job.uid_length);
        } else {
            command = DATA_CMD_MF1_CHECK_KEYS_ON_BLOCK; request_size = 9;
            request[0] = entry[0]; request[1] = entry[1]; request[2] = 1;
            memcpy(request + 3, entry + 2, 6);
        }
    }
    status = nfca_prepare(command, request_size, request);
    if (status == STATUS_SUCCESS && job.kind == 0) {
        /* Reads and nonce captures check the UID during their own selection. */
        status = nfca_command(DATA_CMD_HF14A_SCAN, 0, NULL, output, &size);
        if (status == STATUS_HF_TAG_OK && (size < 1u + job.uid_length || output[0] != job.uid_length ||
            memcmp(output + 1, job.request + 3, job.uid_length))) status = STATUS_HF_TAG_NO;
        size = 0;
        if (status == STATUS_HF_TAG_OK) status = STATUS_SUCCESS;
    }
    if (status == STATUS_SUCCESS) {
        status = nfca_command(command, request_size, request, output, &size);
        nfca_finish(command);
    }
    if (size > NFCA_COMMAND_OUTPUT_BYTES) { emit(STATUS_PAR_ERR, NULL, 0, 1); return; }
    bool okay = status == STATUS_HF_TAG_OK || (job.kind != 2 && status == STATUS_MF_ERR_AUTH);
    if (okay) job.completed += advance;
    emit(status, output, size, !okay || job.completed == job.total ? 1 : 0);
}
