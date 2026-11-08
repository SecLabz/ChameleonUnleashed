#include "fds_util.h"
#include "fds_internal_defs.h"
#include "bsp_wdt.h"
#include "app_util.h"
#include "nrf_sdh.h"
#include "nrf_soc.h"

#define NRF_LOG_MODULE_NAME fds_sync
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();


// current write record info
static struct {
    uint32_t record_id; // record id, used for sync delete
    uint16_t id;        // file id
    uint16_t key;       // file key
    bool success;       // task is success
    bool waiting;       // task waiting done.
    bool ignore_pm;     // ignore peer manager records, defaults to true, set to false by fds_wipe
} fds_operation_info;


/**
 *The query record exists, and get the handle of the record
 */
static bool fds_find_record(uint16_t id, uint16_t key, fds_record_desc_t *desc) {
    fds_find_token_t ftok;
    memset(&ftok, 0x00, sizeof(fds_find_token_t));  // You need to be empty before use
    if (fds_record_find(id, key, desc, &ftok) == NRF_SUCCESS) {
        return true;
    }
    return false;
}

/**
 * @brief Determine whether the record exists.
 *
 * @param id record id
 * @param key record key
 * @return true on record exists
 * @return false on no found
 */
bool fds_is_exists(uint16_t id, uint16_t key) {
    fds_record_desc_t record_desc;
    if (fds_find_record(id, key, &record_desc)) {
        return true;
    }
    return false;
}


/**
 *Read record
 * Length: set it to max length (size of buffer)
 * After execution, length is updated to the real flash record size
 */
bool fds_read_sync(uint16_t id, uint16_t key, uint16_t *length, uint8_t *buffer) {
    fds_record_desc_t desc;
    if (!fds_find_record(id, key, &desc)) {
        *length = 0;
        return false;
    }
    fds_flash_record_t record;
    ret_code_t error = fds_record_open(&desc, &record);
    APP_ERROR_CHECK(error);
    size_t size = record.p_header->length_words * 4;
    bool fits = size <= *length;
    if (fits) memcpy(buffer, record.p_data, size);
    error = fds_record_close(&desc);
    APP_ERROR_CHECK(error);
    *length = fits ? size : 0;
    return fits;
}

uint32_t fds_hash_bytes(uint32_t hash, const uint8_t *bytes, size_t size) {
    hash = (hash ^ size) * 16777619u;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

uint32_t fds_hash_sync(uint16_t id, uint16_t key, uint32_t hash) {
    fds_record_desc_t desc;
    if (!fds_find_record(id, key, &desc)) return fds_hash_bytes(hash, NULL, 0);
    fds_flash_record_t record;
    ret_code_t error = fds_record_open(&desc, &record);
    APP_ERROR_CHECK(error);
    hash = fds_hash_bytes(hash, record.p_data, record.p_header->length_words * 4);
    error = fds_record_close(&desc);
    APP_ERROR_CHECK(error);
    return hash;
}

/**
 * There is no realization of the writing operation function of the GC process
 */
static ret_code_t fds_write_record_nogc(uint16_t id, uint16_t key, uint16_t data_length_words, void *buffer) {
    ret_code_t          err_code;       // The results of the operation
    fds_record_desc_t   record_desc;    // Recorded handle
    fds_record_t record = {             // The entity of the record is used for writing and updating the operation.
        .file_id = id, .key = key,
        .data = { .p_data = buffer, .length_words = data_length_words, }
    };
    if (fds_find_record(id, key, &record_desc)) {   // Find a record with specified characteristics
        //If you can find this record, we can perform the update operation
        NRF_LOG_INFO("Search FileID: 0x%04x, FileKey: 0x%04x is found, will update.", id, key);
        err_code = fds_record_update(&record_desc, &record);
        if (err_code != NRF_SUCCESS) {
            NRF_LOG_INFO("Record update request failed!");
        } // Don't NRF_LOG if request succeeded, it would be interrupted by NRF_LOG in record handler
    } else {
        // Unable to find effective records, we will write for the first time
        NRF_LOG_INFO("Search FileID: 0x%04x, FileKey: 0x%04x no found, will create.", id, key);
        err_code = fds_record_write(&record_desc, &record);
        if (err_code != NRF_SUCCESS) {
            NRF_LOG_INFO("Record creation request failed!");
        } // Don't NRF_LOG if request succeeded, it would be interrupted by NRF_LOG in record handler
    }
    return err_code;
}

/**
 * Write record
 */
bool fds_write_sync(uint16_t id, uint16_t key, uint16_t length, void *buffer) {
    // Make only one task running
    APP_ERROR_CHECK_BOOL(!fds_operation_info.waiting);
    // write result
    bool ret = true;
    // write or update record info cache
    fds_operation_info.id = id;
    fds_operation_info.key = key;
    fds_operation_info.success = false;
    fds_operation_info.waiting = true;
    // compute needed words
    if (length == 0) {
        return ret;
    }
    uint16_t data_length_words = ((length - 1) / 4) + 1;

    // CCall the write implementation function without automatic GC
    ret_code_t err_code = fds_write_record_nogc(id, key, data_length_words, buffer);
    if (err_code == NRF_SUCCESS) {
        while (!fds_operation_info.success) {
            __NOP();
        }; // Waiting for operation to complete
    } else if (err_code == FDS_ERR_NO_SPACE_IN_FLASH) {   //Make sure there is space to operate, otherwise GC will be required
        // The current error is an error with insufficient space. Maybe we need GC
        NRF_LOG_INFO("FDS no space, gc auto start.");
        fds_gc_sync();

        // After the GC is completed, it can be re -operated
        NRF_LOG_INFO("FDS auto gc success, write record continue.");
        fds_operation_info.success = false;
        err_code = fds_write_record_nogc(id, key, data_length_words, buffer);
        if (err_code == NRF_SUCCESS) {
            while (!fds_operation_info.success) {
                __NOP();
            }; // Waiting for operation to complete
        } else if (err_code == FDS_ERR_NO_SPACE_IN_FLASH) {
            //After gc once, I found that there is still no space, so it may be that the developer did not consider the space distribution and caused overflow
            NRF_LOG_ERROR("FDS no space to write.");
            ret = false;
        } else {
            //If it is not an error with insufficient space, then we need Catch the error, and we solve it during development
            APP_ERROR_CHECK(err_code);
        }
    } else {
        // Above
        APP_ERROR_CHECK(err_code);
    }

    // task process finish
    fds_operation_info.waiting = false;
    return ret;
}

/*
 * Delete Record
 */
int fds_delete_sync(uint16_t id, uint16_t key) {
    int                 delete_count = 0;
    fds_record_desc_t   record_desc;
    ret_code_t          err_code;
    while (fds_find_record(id, key, &record_desc)) {
        fds_operation_info.success = false;
        fds_record_id_from_desc(&record_desc, &fds_operation_info.record_id);
        err_code = fds_record_delete(&record_desc);
        APP_ERROR_CHECK(err_code);
        delete_count++;
        while (!fds_operation_info.success) {
            __NOP();
        }; //Waiting for operation to complete
    }
    return delete_count;
}

static bool is_peer_manager_record(uint16_t id_or_key) {
    if (id_or_key > 0xBFFF) {
        return true;
    }
    return false;
}

/**
 *FDS event callback
 */
static void fds_evt_handler(fds_evt_t const *p_evt) {
    // Skip peermanager event
    if (fds_operation_info.ignore_pm && (
                is_peer_manager_record(p_evt->write.record_key)
                || is_peer_manager_record(p_evt->write.file_id)
                || is_peer_manager_record(p_evt->del.record_key)
                || is_peer_manager_record(p_evt->del.file_id)
            )
       ) {
        return;
    }

    // To process fds event
    switch (p_evt->id) {
        case FDS_EVT_INIT: {
            if (p_evt->result == NRF_SUCCESS) {
                NRF_LOG_INFO("NRF52 FDS libraries init success.");
            } else {
                NRF_LOG_INFO("NRF52 FDS libraries init failed");
                APP_ERROR_CHECK(p_evt->result);
            }
        }
        break;
        case FDS_EVT_WRITE:
        case FDS_EVT_UPDATE: {
            if (p_evt->result == NRF_SUCCESS) {
                NRF_LOG_INFO("Record change: FileID 0x%04x, RecordKey 0x%04x", p_evt->write.file_id, p_evt->write.record_key);
                if (p_evt->write.file_id == fds_operation_info.id && p_evt->write.record_key == fds_operation_info.key) {
                    // The logic above has ensured that the task we are currently writing is completed!
                    NRF_LOG_INFO("Record change success");
                    fds_operation_info.success = true;
                } else NRF_LOG_INFO("Record change mismatch");
            } else {
                NRF_LOG_INFO("Record change failed");
                APP_ERROR_CHECK(p_evt->result);
            }
        }
        break;
        case FDS_EVT_DEL_RECORD: {
            if (p_evt->result == NRF_SUCCESS) {
                NRF_LOG_INFO(
                    "Record remove: FileID: 0x%04x, RecordKey: 0x%04x, RecordID: %08x",
                    p_evt->del.file_id, p_evt->del.record_key, p_evt->del.record_id
                );
                if (p_evt->del.record_id == fds_operation_info.record_id) {
                    // Only check record id because fileID and recordKey aren't available
                    // if deleting via fds_record_iterate. record id is guaranteed to be unique.
                    NRF_LOG_INFO("Record delete success");
                    fds_operation_info.success = true;
                } else NRF_LOG_INFO("Record delete mismatch");
            } else {
                NRF_LOG_INFO("Record delete failed");
                APP_ERROR_CHECK(p_evt->result);
            }
        }
        break;
        case FDS_EVT_GC: {
            if (p_evt->result == NRF_SUCCESS) {
                NRF_LOG_INFO("FDS gc success");
                fds_operation_info.success = true;
            } else {
                NRF_LOG_INFO("FDS gc failed");
                APP_ERROR_CHECK(p_evt->result);
            }
        }
        break;
        default: {
            // nothing to do...
        } break;
    }
}

static bool fds_recover_empty_swap(void) {
    /* Promoting an empty page to swap must leave at least one data page. */
    enum { MIN_DATA_PAGES_FOR_REPAIR = 2 };
    /* A legacy DFU bootloader can stage its replacement over the old swap.
     * Reuse only a data page with no records at all. Never erase a record or
     * an unrecognized page, and never issue synchronous writes with SD on. */
    if (nrf_sdh_is_enabled()) return false;
    uintptr_t end = BOOTLOADER_ADDRESS;
    size_t page_bytes = FDS_VIRTUAL_PAGE_SIZE * sizeof(uint32_t);
    size_t physical_bytes = NRF_FICR->CODEPAGESIZE;
    size_t reserved = FDS_VIRTUAL_PAGES_RESERVED * page_bytes;
    if (!physical_bytes || page_bytes % physical_bytes ||
        end == FDS_ERASED_WORD || end < reserved + FDS_VIRTUAL_PAGES * page_bytes)
        return false;
    uintptr_t start = end - reserved - FDS_VIRTUAL_PAGES * page_bytes;
    uint32_t *empty = NULL;
    unsigned data_pages = 0;
    for (unsigned i = 0; i < FDS_VIRTUAL_PAGES; i++) {
        uint32_t *page = (uint32_t *)(start + i * page_bytes);
        if (page[FDS_PAGE_TAG_WORD_0] == FDS_ERASED_WORD || page[FDS_PAGE_TAG_WORD_0] == FDS_PAGE_TAG_MAGIC) {
            unsigned word = FDS_PAGE_TAG_WORD_1;
            while (word < FDS_VIRTUAL_PAGE_SIZE && page[word] == FDS_ERASED_WORD) word++;
            /* FDS already knows how to use an erased or partially tagged page. */
            if (word == FDS_VIRTUAL_PAGE_SIZE) return false;
        }
        if (page[FDS_PAGE_TAG_WORD_0] != FDS_PAGE_TAG_MAGIC) continue;
        if (page[FDS_PAGE_TAG_WORD_1] == FDS_PAGE_TAG_SWAP) return false;
        if (page[FDS_PAGE_TAG_WORD_1] != FDS_PAGE_TAG_DATA) continue;
        data_pages++;
        unsigned word = FDS_PAGE_TAG_SIZE;
        while (word < FDS_VIRTUAL_PAGE_SIZE && page[word] == FDS_ERASED_WORD) word++;
        if (word == FDS_VIRTUAL_PAGE_SIZE) empty = page;
    }
    if (data_pages < MIN_DATA_PAGES_FOR_REPAIR || !empty) return false;
    /* Only the first physical page holds the tag; the rest is already erased.
     * A reset after this erase leaves a blank page FDS can tag on its own. */
    if (sd_flash_page_erase((uintptr_t)empty / physical_bytes) != NRF_SUCCESS)
        return false;
    uint32_t tag[FDS_PAGE_TAG_SIZE] = {FDS_PAGE_TAG_MAGIC, FDS_PAGE_TAG_SWAP};
    if (sd_flash_write(empty, tag, FDS_PAGE_TAG_SIZE) != NRF_SUCCESS) return false;
    return memcmp(empty, tag, sizeof(tag)) == 0;
}

/** Initialize FDS after recovering a missing swap when an empty data page exists. */
void fds_util_init() {
    fds_operation_info.ignore_pm = true;
    // reset waiting flag
    fds_operation_info.waiting = false;
    //Register the incident first
    ret_code_t err_code = fds_register(fds_evt_handler);
    APP_ERROR_CHECK(err_code);
    /* Scan before fds_init(): SDK pages_init() assumes one swap exists and can
     * otherwise index past its FDS_VIRTUAL_PAGES - 1 data-page array. */
    (void)fds_recover_empty_swap();
    //Start the initialization FDS library
    err_code = fds_init();
    APP_ERROR_CHECK(err_code);
}

void fds_gc_sync(void) {
    fds_operation_info.success = false;
    ret_code_t err_code = fds_gc();
    APP_ERROR_CHECK(err_code);
    while (!fds_operation_info.success) {
        __NOP();
    };
}

static bool fds_next_record_delete_sync() {
    fds_find_token_t  tok   = {0};
    fds_record_desc_t desc  = {0};
    if (fds_record_iterate(&desc, &tok) != NRF_SUCCESS) {
        NRF_LOG_INFO("No more records to delete");
        return false;
    }

    fds_record_id_from_desc(&desc, &fds_operation_info.record_id);
    NRF_LOG_INFO("Deleting record with id=%08x", fds_operation_info.record_id);

    fds_operation_info.success = false;
    ret_code_t rc = fds_record_delete(&desc);
    if (rc != NRF_SUCCESS) {
        NRF_LOG_WARNING("Record id=%08x deletion failed with rc=%d!", fds_operation_info.record_id, rc);
        return false;
    }

    while (!fds_operation_info.success) {
        __NOP();
    }

    NRF_LOG_INFO("Record id=%08x deleted successfully", fds_operation_info.record_id);
    return true;
}

bool fds_wipe(void) {
    NRF_LOG_INFO("Full fds wipe requested");
    fds_operation_info.ignore_pm = false;  // wipe should also delete peer manager files.
    while (fds_next_record_delete_sync()) {
        bsp_wdt_feed();
    }
    fds_gc_sync();
    return true;
}
