#pragma once

#include <stddef.h>
#include <stdint.h>

/* Command 3033, version 1. All integers are big-endian.
 * Request: version, operation (0 inspect / 1 prepare / 2 program), five zero
 * reserved bytes. Prepare/program append type:u16, EM clock:u8 (0/16/32/64),
 * image length:u8, existing LF image bytes; program appends fingerprint:u32.
 * Password-mode, extended, AOR and OTP targets are not programmed.
 * Snapshot: version, detected, readable mask, result (0 observed / 1 unchanged /
 * 2 programmed and verified), eight u32 words; password word 7 stays zero.
 * Plan: snapshot, word count:u8, seven desired u32 words, fingerprint:u32.
 * The fingerprint binds the configuration and words needed by this plan;
 * equal contents are indistinguishable. It is not a silicon UID. */
#define LF_T55XX_VERSION 1
#define LF_T55XX_SNAPSHOT_BYTES 36
#define LF_T55XX_PLAN_BYTES 69

uint16_t lf_t55xx_command(const uint8_t *request, size_t size,
    uint8_t *output, uint16_t *length);
