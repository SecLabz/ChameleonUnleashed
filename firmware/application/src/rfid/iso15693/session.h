#pragma once
#include "frame.h"
#include "memory.h"
#include "reply.h"
/* Legacy storage capacities; RF exposes 8/28 user blocks. */
#define NFCV_SLIX_L_BLOCKS 16u
#define NFCV_SLIX_BLOCKS 32u
#define NFCV_IMAGE_HEADER_SIZE 32u
#define NFCV_IMAGE_SIZE(blocks) (NFCV_IMAGE_HEADER_SIZE + (blocks) * (NFCV_BLOCK_SIZE + 1u))
#define NFCV_IMAGE_MAX_SIZE NFCV_IMAGE_SIZE(NFCV_SLIX_BLOCKS)

/* Configure successfully before session_init, and keep the original alive and unchanged.
 * Field-loss resets retain mutable RF memory and do not reread the original. */
bool nfcv_image_valid(const uint8_t *image, bool slix_l);
bool nfcv_session_configure(const uint8_t *image, bool slix_l);
void nfcv_session_save(uint8_t *image);
void nfcv_session_init(void);
void nfcv_session_field_lost(void);
bool nfcv_session_slot_pending(void);
bool nfcv_session_high_rate(void);
bool nfcv_session_next_slot(nfcv_reply_t *response);
bool nfcv_session_handle(const iso15693_frame_t *frame, nfcv_reply_t *response);
