/* Replay captured RC522 ADC banks through firmware receiver.c, as stb_window does.
 *
 * Capture layout: 76-byte banks {65 ADC bytes (I high nibble, Q low nibble),
 * skip, half, carry, valid, 3 pad bytes, u32 receiver time in 2 us pairs}.
 * valid: 1 = bank, 2 = stream restart, 3 = restart after a reply (receiver settle).
 * Build and run with replay.sh.
 */
#include "st25tb/receiver.h"
#include <stdio.h>

typedef struct {
    uint8_t data[65];
    uint8_t skip;
    uint8_t half;
    uint8_t carry;
    uint8_t valid;
    uint8_t reserved[3];
    uint8_t time[4];
} bank_t;

_Static_assert(sizeof(bank_t) == 76, "capture layout");

static unsigned replay(const bank_t *banks, size_t count) {
    uint8_t frame[ST25TB_REQUEST_MAX];
    unsigned length;
    unsigned frames = 0;
    st25tb_receiver_reset();
    for (size_t b = 0; b < count; b++) {
        if (banks[b].valid == 2 || banks[b].valid == 3) {
            st25tb_receiver_reset();
            if (banks[b].valid == 3) {
                st25tb_receiver_settle();
            }
            continue;
        }
        if (banks[b].valid != 1) {
            continue;
        }
        const uint8_t *time = banks[b].time;
        st25tb_receiver_time((unsigned)time[0] | (unsigned)time[1] << 8 | (unsigned)time[2] << 16 |
                             (unsigned)time[3] << 24);
        bool half = banks[b].half;
        for (unsigned n = banks[b].skip; n <= 64;) {
            length = 0;
            if (half) {
                half = false;
                st25tb_receiver_pair(banks[b].carry, banks[b].data[n++], frame, &length);
            } else {
                unsigned bytes = 65 - n;
                if (bytes == 1) {
                    break;
                }
                bytes &= ~1u;
                n += st25tb_receiver_batch(banks[b].data + n, bytes > 64 ? 64 : bytes, frame, &length);
            }
            if (!length) {
                continue;
            }
            frames++;
            printf(" [");
            for (unsigned i = 0; i < length; i++) {
                printf("%s%02x", i ? " " : "", frame[i]);
            }
            printf("]");
        }
    }
    return frames;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: replay CAPTURE.bin...\n");
        return 1;
    }
    int decoded = 0;
    for (int f = 1; f < argc; f++) {
        FILE *file = fopen(argv[f], "rb");
        if (!file) {
            perror(argv[f]);
            return 1;
        }
        /* Full measured requests can exceed a short 64-bank snapshot. */
        bank_t banks[512];
        size_t bytes = fread(banks, 1, sizeof(banks), file);
        bool invalid = ferror(file) || !bytes || bytes % sizeof(bank_t) || fgetc(file) != EOF;
        fclose(file);
        size_t count = bytes / sizeof(bank_t);
        for (size_t bank = 0; bank < count; bank++) {
            if (banks[bank].valid > 3 || banks[bank].skip > 64 || banks[bank].half > 1) {
                invalid = true;
            }
        }
        if (invalid) {
            fprintf(stderr, "%s: invalid or truncated capture\n", argv[f]);
            return 1;
        }
        printf("%-48s", argv[f]);
        unsigned frames = replay(banks, count);
        printf("%s\n", frames ? "" : " LOST");
        decoded += frames > 0;
    }
    printf("decoded %d/%d\n", decoded, argc - 1);
    return 0;
}
