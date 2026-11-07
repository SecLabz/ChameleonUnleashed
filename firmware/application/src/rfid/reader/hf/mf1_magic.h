#ifndef MF1_MAGIC_H
#define MF1_MAGIC_H

#include <stdint.h>

enum {
    MF1_MAGIC_GEN1A = 1u << 0,
    MF1_MAGIC_GEN1B = 1u << 1,
    MF1_MAGIC_GEN2 = 1u << 2,
    MF1_MAGIC_UNFUSED = 1u << 3,
    MF1_MAGIC_SUPER_GEN1 = 1u << 4,
    MF1_MAGIC_SUPER_GEN2 = 1u << 5,
    MF1_MAGIC_GEN3 = 1u << 7,
    MF1_MAGIC_GTU = 1u << 8,
    MF1_MAGIC_GDM_AUTH = 1u << 9,
    MF1_MAGIC_QL88 = 1u << 10,
    MF1_MAGIC_GDM_20 = 1u << 11,
    MF1_MAGIC_GDM_40 = 1u << 12,
    MF1_MAGIC_GDM_ZUID = 1u << 13,
};

enum {
    MF1_MAGIC_GEN2_NOT_CHECKED,
    MF1_MAGIC_GEN2_AUTH_DENIED,
    MF1_MAGIC_GEN2_WRITE_DENIED,
    MF1_MAGIC_GEN2_WRITE_ACK,
    MF1_MAGIC_GEN2_ATS,
    MF1_MAGIC_GEN2_INCONCLUSIVE,
};

/* Command 2032: UID length, ten zero-padded UID bytes, two RF-order ATQA
 * bytes, SAK, credential (0/1/2 = none/A/B), optional six-byte key.
 * Reply: big-endian signature mask and Gen2 evidence byte. No write data
 * is sent. Independent probes reset RF and validate UID/ATQA/SAK. The caller
 * owns prepare/finish, including invalid requests. Probes leave the field off
 * and Crypto1 disabled, restoring timeout and activation configuration. */
uint16_t mf1_magic_probe(uint16_t length, const uint8_t *data,
                         uint8_t *output, uint16_t *output_length);

/* Command 2040: fourteen identity bytes as in 2032, key count (1..64),
 * then six bytes per essential key in priority order. Reply: the unchanged
 * three-byte magic evidence, PRNG (0..2), verified credential (0/1/2), key[6]
 * (zero when absent). No memory image is read and no write data is sent.
 * Success binds all observations to the supplied UID/ATQA/SAK. An optional
 * trailing request byte 1 appends evidence-version 1, ATS length (0..64), and
 * that many raw ATS bytes including CRC-A. Legacy requests retain 11 bytes.
 * ATS comes from the existing RATS probe; invalid TL/CRC/bit lengths return
 * no ATS. Later identity failures suppress the entire reply. */
uint16_t mf1_classic_inspect(uint16_t length, const uint8_t *data,
                             uint8_t *output, uint16_t *output_length);

/* Read-only Plus S/X SL1 2K capacity evidence for guarded mutations. Require
 * SAK 08, matching UID/ATQA layout and a valid known ATS (TL, CRC, history).
 * Reselect the same UID/ATQA/SAK before returning success in Classic state.
 * Failure leaves RF off and Crypto1 disabled. Restore timeout/configuration;
 * no authentication, memory read, WRITE opcode or write data is sent. */
uint16_t mf1_classic_2k_sl1_check(uint8_t uid_length, const uint8_t *uid,
                                 const uint8_t *atqa, uint8_t sak);

#endif
