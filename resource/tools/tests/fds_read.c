#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef unsigned ret_code_t;
typedef struct { unsigned id; } fds_record_desc_t;
typedef struct { unsigned length_words; } record_header_t;
typedef struct { const record_header_t *p_header; const void *p_data; } fds_flash_record_t;
static const record_header_t header = {2};
static const uint8_t stored[8] = {1,2,3,4,5,6,7,8};
static bool exists = true;
static unsigned opens, closes;
static fds_record_desc_t *opened;
#define APP_ERROR_CHECK(error) assert((error) == 0)
static bool fds_find_record(uint16_t id, uint16_t key, fds_record_desc_t *desc) {
    desc->id = id; return exists;
}
static ret_code_t fds_record_open(fds_record_desc_t *desc, fds_flash_record_t *record) {
    assert(!opened); opened = desc; opens++;
    *record = (fds_flash_record_t){&header, stored}; return 0;
}
static ret_code_t fds_record_close(fds_record_desc_t *desc) {
    assert(opened == desc); opened = NULL; closes++; return 0;
}
/* PRODUCTION_READ */
int main(void) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 100; i++) {
        uint16_t size = sizeof(bytes);
        assert(fds_read_sync(1,2,&size,bytes));
        assert(size == 8 && !memcmp(bytes,stored,8) && !opened);
    }
    assert(opens == 100 && closes == 100);
    uint16_t size = 4; memset(bytes,42,8);
    assert(!fds_read_sync(1,2,&size,bytes) && size == 0 && !opened);
    assert(opens == 101 && closes == 101 && bytes[0] == 42);
    uint32_t hash=fds_hash_sync(1,2,2166136261u);
    assert(hash==fds_hash_bytes(2166136261u,stored,8) && !opened && opens==102 && closes==102);
    exists = false; size = 8;
    assert(!fds_read_sync(1,2,&size,bytes) && size == 0 && opens == 102 && closes == 102);
    assert(fds_hash_sync(1,2,2166136261u)!=hash && !opened);
}
