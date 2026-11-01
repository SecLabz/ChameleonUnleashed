#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "standard.h"

typedef enum {
    SLIX_PASSWORD_TYPE_READ = 0u,
    SLIX_PASSWORD_TYPE_WRITE,
    SLIX_PASSWORD_TYPE_PRIVACY,
    SLIX_PASSWORD_TYPE_DESTROY,
    SLIX_PASSWORD_TYPE_EAS_AFI,
    SLIX_PASSWORD_TYPE_COUNT,
} slix_password_type_t;

typedef struct {
    bool eas_active;
    bool eas_locked;
    bool privacy_mode;
    bool password_match[SLIX_PASSWORD_TYPE_COUNT];
    uint32_t passwords[SLIX_PASSWORD_TYPE_COUNT];
    uint16_t pseudo_random;
    nfcv_std_t *standard;
    bool slix_l;
} slix_t;

void slix_init(slix_t *slix, nfcv_std_t *standard, bool eas_locked, bool privacy_mode,
               const uint32_t passwords[SLIX_PASSWORD_TYPE_COUNT], bool slix_l);
void slix_reset_session(slix_t *slix);
bool slix_allows_command(const slix_t *slix, uint8_t command);
void slix_handle(slix_t *slix, const nfcv_request_t *request, nfcv_reply_t *response);
