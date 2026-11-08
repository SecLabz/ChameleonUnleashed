#include "slot_api.h"
#include "slot_image.h"
#include "desfire_model.h"
#include "app_status.h"
#include "fds_util.h"
#include "rfid_main.h"
#include "tag_persistence.h"
#include <string.h>

static uint8_t wire[SLOT_IMAGE_MAX_BYTES];
static uint32_t native_words[(SLOT_IMAGE_MAX_BYTES + 3) / 4];
static uint16_t total, received;
static uint32_t expected_revision;
static uint8_t target;
static tag_sense_type_t target_sense;
static enum { EMPTY, SNAPSHOT, SERIAL_SNAPSHOT, UPLOAD } state;
static uint32_t snapshot_id;

void web_slots_reset(void) {
    memset(wire, 0, sizeof(wire));
    memset(native_words, 0, sizeof(native_words));
    state = EMPTY; total = received = 0;
}

static uint32_t read32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}
static unsigned read16(const uint8_t *bytes) { return (bytes[0] << 8) | bytes[1]; }
static void write16(uint8_t *bytes, unsigned value) { bytes[0] = value >> 8; bytes[1] = value; }
static api_response_t response(unsigned status, size_t size) {
    return (api_response_t){status, "application/octet-stream", size};
}
static api_response_t outcome(bool success, uint8_t *output) {
    output[0] = 0; output[1] = success ? STATUS_SUCCESS : STATUS_FLASH_WRITE_FAIL;
    return response(200, 2);
}

static bool valid_target(const uint8_t *body, size_t size) {
    return size >= 2 && body[0] < TAG_MAX_SLOT_NUM &&
        (body[1] == TAG_SENSE_HF || body[1] == TAG_SENSE_LF);
}

static bool valid_name(const uint8_t *bytes, size_t size) {
    if (!size || size > 32) return false;
    for (size_t i = 0; i < size;) {
        unsigned c = bytes[i++], more, value;
        if (c < 0x80) { if (c < 32 || c == 127) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { more = 1; value = c & 31; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; value = c & 15; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; value = c & 7; }
        else return false;
        if (i + more > size) return false;
        unsigned count = more;
        while (more--) {
            unsigned next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if ((count == 2 && value < 0x800) || (count == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool save_name(uint8_t slot, tag_sense_type_t sense, const uint8_t *name, size_t size) {
    uint8_t bytes[36] = {0};
    bytes[0] = size; memcpy(bytes + 1, name, size);
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_nick(slot, sense, &map);
    if (!fds_write_sync(map.id, map.key, sizeof(bytes), bytes)) return false;
    uint8_t saved[36]; uint16_t length = sizeof(saved);
    return fds_read_sync(map.id, map.key, &length, saved) && length == sizeof(saved) &&
        !memcmp(saved, bytes, sizeof(bytes));
}

static bool valid_saved_image(uint8_t slot, tag_sense_type_t sense, tag_specific_type_t type) {
    if (type == TAG_TYPE_UNDEFINED) return true;
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_dump(slot, sense, &map);
    uint16_t length = sizeof(native_words), encoded;
    if (!fds_read_sync(map.id, map.key, &length, (uint8_t *)native_words)) return false;
    slot_image_shape_t shape;
    // Other native families retain their own load callback's size validation.
    if (!slot_image_shape(type, &shape)) return true;
    if (type == TAG_TYPE_DESFIRE) shape.native_bytes = desfire_image_size((uint8_t *)native_words, length);
    return length == shape.native_bytes &&
        slot_image_encode(type, (uint8_t *)native_words, length, wire, sizeof(wire), &encoded) &&
        slot_image_decode(wire, encoded, (uint8_t *)native_words, sizeof(native_words), &shape);
}

static bool capture(uint8_t slot, tag_sense_type_t sense) {
    web_slots_reset();
    uint16_t length = sizeof(native_words);
    tag_specific_type_t type;
    bool saved = tag_emulation_snapshot(slot, sense, (uint8_t *)native_words, &length, &type) &&
        slot_image_encode(type, (uint8_t *)native_words, length, wire, sizeof(wire), &total);
    memset(native_words, 0, sizeof(native_words));
    return saved;
}

/* Serial: open [slot,sense] -> [id BE32,size BE16], read [id,offset BE16,
 * count BE16] -> bytes, close [id] -> empty. HTTP and serial share the same
 * storage; a reset or another open invalidates the previous serial id. */
uint16_t slot_snapshot_request(const uint8_t *body, size_t size, uint8_t *output,
    size_t capacity, uint16_t *length) {
    *length = 0;
    if (size == 2) {
        if (!valid_target(body, size) || capacity < 6) {
            return STATUS_PAR_ERR;
        }
        if (g_is_tag_emulating || snapshot_id == UINT32_MAX) {
            return STATUS_BUSY;
        }
        if (!capture(body[0], body[1])) {
            return g_is_tag_emulating ? STATUS_BUSY : STATUS_INVALID_SLOT_TYPE;
        }
        snapshot_id++;
        state = SERIAL_SNAPSHOT;
        for (unsigned i = 0; i < 4; i++) {
            output[i] = snapshot_id >> (24 - 8 * i);
        }
        write16(output + 4, total);
        *length = 6;
        return STATUS_SUCCESS;
    }
    if (size != 4 && size != 8) {
        return STATUS_PAR_ERR;
    }
    if (state != SERIAL_SNAPSHOT || read32(body) != snapshot_id) {
        return STATUS_BUSY;
    }
    if (size == 4) {
        web_slots_reset();
        return STATUS_SUCCESS;
    }
    unsigned offset = read16(body + 4);
    unsigned count = read16(body + 6);
    if (!count || count > capacity || offset + count > total) {
        return STATUS_PAR_ERR;
    }
    memcpy(output, wire + offset, count);
    *length = count;
    return STATUS_SUCCESS;
}

static api_response_t open_snapshot(const uint8_t *body, size_t size, uint8_t *output) {
    if ((size != 2 && size != 6) || !valid_target(body, size)) return response(400, 0);
    if (size == 6 && read32(body + 2) != tag_emulation_place_revision(body[0], body[1])) return response(409, 0);
    if (!capture(body[0], body[1])) return response(422, 0);
    state = SNAPSHOT;
    write16(output, total);
    uint32_t revision = tag_emulation_place_revision(body[0], body[1]);
    for (unsigned i = 0; i < 4; i++) output[2 + i] = revision >> (24 - 8 * i);
    return response(200, 6);
}

static api_response_t commit_upload(const uint8_t *body, size_t size, uint8_t *output) {
    if (state != UPLOAD || received != total) return response(409, 0);
    if (size < 3 || body[0] > 1 || size != (size_t)body[1] + 2 || !valid_name(body + 2, body[1])) return response(400, 0);
    if (expected_revision != tag_emulation_place_revision(target, target_sense)) {
        web_slots_reset(); return response(409, 0);
    }
    slot_image_shape_t shape;
    if (!slot_image_decode(wire, total, (uint8_t *)native_words, sizeof(native_words), &shape) || shape.sense != target_sense) return response(400, 0);
    state = EMPTY;
    /* Persistent disable comes first: interrupted replacement must not start
     * an old type against a new buffer. The other frequency is untouched. */
    if (!tag_emulation_slot_configure(target, target_sense, TAG_TYPE_UNDEFINED, false, wire, sizeof(wire))) {
        web_slots_reset();
        return outcome(false, output);
    }
    fds_slot_record_map_t map;
    get_fds_map_by_slot_sense_type_for_dump(target, target_sense, &map);
    bool saved = fds_write_sync(map.id, map.key, shape.native_bytes, native_words);
    if (saved) {
        uint16_t length = sizeof(wire);
        saved = fds_read_sync(map.id, map.key, &length, wire) && length == shape.native_bytes &&
            !memcmp(wire, native_words, length);
    }
    if (saved) saved = save_name(target, target_sense, body + 2, body[1]);
    if (saved) saved = tag_emulation_slot_configure(target, target_sense, shape.type, body[0], wire, sizeof(wire));
    web_slots_reset();
    return outcome(saved, output);
}

static api_response_t slot_request(api_operation_t operation, const uint8_t *body,
    size_t size, uint8_t *output, size_t capacity) {
    if (capacity < 6) return response(500, 0);
    if (operation == API_SLOT_OPEN) return open_snapshot(body, size, output);
    if (operation == API_SLOT_READ) {
        if (state != SNAPSHOT) return response(409, 0);
        if (size != 4) return response(400, 0);
        unsigned offset = read16(body), count = read16(body + 2);
        if (!count || count > capacity || offset + count > total) return response(400, 0);
        memcpy(output, wire + offset, count);
        return response(200, count);
    }
    if (operation == API_SLOT_BEGIN) {
        if (size != 8 || !valid_target(body, size)) return response(400, 0);
        uint32_t revision = read32(body + 4);
        if (revision != tag_emulation_place_revision(body[0], body[1])) return response(409, 0);
        unsigned length = read16(body + 2);
        if (length < 9 || length > sizeof(wire)) return response(400, 0);
        web_slots_reset();
        target = body[0]; target_sense = body[1]; total = length; expected_revision = revision; state = UPLOAD;
        return response(200, 0);
    }
    if (operation == API_SLOT_WRITE) {
        if (state != UPLOAD) return response(409, 0);
        if (size <= 2 || size > 1024 || read16(body) != received || received + size - 2 > total) return response(400, 0);
        memcpy(wire + received, body + 2, size - 2); received += size - 2;
        write16(output, received);
        return response(200, 2);
    }
    if (operation == API_SLOT_COMMIT) return commit_upload(body, size, output);
    if (state == UPLOAD) return response(409, 0);
    if (operation == API_SLOT_ACTIVATE) {
        if (size != 9 || body[0] >= TAG_MAX_SLOT_NUM) return response(400, 0);
        if (read32(body + 1) != tag_emulation_place_revision(body[0], TAG_SENSE_HF) ||
            read32(body + 5) != tag_emulation_place_revision(body[0], TAG_SENSE_LF)) return response(409, 0);
        web_slots_reset();
        tag_slot_specific_type_t types;
        tag_emulation_get_specific_types_by_slot(body[0], &types);
        if (!valid_saved_image(body[0], TAG_SENSE_HF, types.tag_hf) ||
            !valid_saved_image(body[0], TAG_SENSE_LF, types.tag_lf)) {
            web_slots_reset(); return response(422, 0);
        }
        bool saved = tag_emulation_select_checked(body[0], (uint8_t *)native_words, sizeof(native_words));
        if (saved) tag_mode_enter();
        web_slots_reset();
        return outcome(saved, output);
    }
    if (operation == API_SLOT_NICK) {
        if (size < 7 || !valid_target(body, size) || !valid_name(body + 6, size - 6)) return response(400, 0);
        if (read32(body + 2) != tag_emulation_place_revision(body[0], body[1])) return response(409, 0);
        return outcome(save_name(body[0], body[1], body + 6, size - 6), output);
    }
    if (operation == API_SLOT_ENABLE) {
        if (size != 7 || !valid_target(body, size) || body[6] > 1) return response(400, 0);
        if (read32(body + 2) != tag_emulation_place_revision(body[0], body[1])) return response(409, 0);
        tag_slot_specific_type_t types;
        tag_emulation_get_specific_types_by_slot(body[0], &types);
        tag_specific_type_t type = body[1] == TAG_SENSE_HF ? types.tag_hf : types.tag_lf;
        if (type == TAG_TYPE_UNDEFINED) return response(422, 0);
        web_slots_reset();
        if (body[6] && !valid_saved_image(body[0], body[1], type)) {
            web_slots_reset(); return response(422, 0);
        }
        bool saved = tag_emulation_slot_configure(body[0], body[1], type, body[6], wire, sizeof(wire));
        web_slots_reset();
        return outcome(saved, output);
    }
    if (operation == API_SLOT_CLEAR) {
        if (size != 6 || !valid_target(body, size)) return response(400, 0);
        if (read32(body + 2) != tag_emulation_place_revision(body[0], body[1])) return response(409, 0);
        web_slots_reset();
        bool saved = tag_emulation_slot_configure(body[0], body[1], TAG_TYPE_UNDEFINED, false, wire, sizeof(wire));
        fds_slot_record_map_t dump, nick;
        get_fds_map_by_slot_sense_type_for_dump(body[0], body[1], &dump);
        get_fds_map_by_slot_sense_type_for_nick(body[0], body[1], &nick);
        if (saved) {
            fds_delete_sync(dump.id, dump.key); fds_delete_sync(nick.id, nick.key);
            saved = !fds_is_exists(dump.id, dump.key) && !fds_is_exists(nick.id, nick.key);
        }
        web_slots_reset();
        return outcome(saved, output);
    }
    return response(404, 0);
}

api_response_t web_slots_request(api_operation_t operation, const uint8_t *body,
    size_t size, uint8_t *output, size_t capacity) {
    tag_emulation_sense_hold(true);
    api_response_t result = slot_request(operation, body, size, output, capacity);
    tag_emulation_sense_hold(false);
    return result;
}
