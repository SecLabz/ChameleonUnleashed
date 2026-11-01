#include "desfire_model.h"
#include "desfire_protocol.h"
#include <string.h>

/* Native slot envelope and capture layout. Cursor pointers borrow the image;
 * successful loading does not copy metadata or file contents into the model. */
enum {
    IMAGE_HEADER_BYTES = 5,
    IMAGE_MIN_BYTES = 10,
    IMAGE_MAGIC_BYTES = 4,
    CAPTURE_HEADER_BYTES = 5,
    CAPTURE_VERSION_OFFSET = 4,
    CAPTURE_VERSION = 1,
    CAPTURE_DATA_MIN_BYTES = 6,
    CAPTURE_METADATA_MIN_BYTES = 10,
    CAPTURE_METADATA_MAX_BYTES = 96,
    CAPTURE_FILE_SETTINGS_MAX_BYTES = 96,
    UID_SINGLE_BYTES = 4,
    UID_DOUBLE_BYTES = 7,
    UID_TRIPLE_BYTES = 10,
    ATQA_UID_SIZE_MASK = 0xc0,
    ATQA_UID_DOUBLE = 0x40,
    ATQA_UID_TRIPLE = 0x80,
    ATS_RESERVED_FORMAT = 0x80,
    ATS_FRAME_SIZE_MASK = 15,
    ATS_FRAME_SIZE_MAX = 8,
    KEY_SETTINGS_BYTES = 2,
    KEY_COUNT_MAX = 14,
    KEY_COUNT_MASK = 15,
    FILES_PER_APPLICATION_MAX = 32,
    FILE_ID_MAX = 31,
    FILE_SETTINGS_COMMON_BYTES = 4,
    DATA_FILE_SETTINGS_BYTES = 7,
    VALUE_FILE_SETTINGS_BYTES = 17,
    RECORD_FILE_SETTINGS_BYTES = 13,
    FILE_CONTENT_SIZE_OFFSET = 4,
    RECORD_CAPACITY_OFFSET = 7,
    RECORD_COUNT_OFFSET = 10,
    VALUE_CONTENT_BYTES = 4,
    VERSION_BYTES = 28,
    VERSION_PART_BYTES = 7,
    VERSION_FINAL_BYTES = 14,
    FREE_MEMORY_MAX = 0xffffff,
};

typedef struct {
    const uint8_t *p;
    unsigned left;
    bool ok;
} cursor_t;

static const uint8_t *cursor_take(cursor_t *c, unsigned n) {
    if (n > c->left) {
        c->ok = false;
        return NULL;
    }
    const uint8_t *p = c->p;
    c->p += n;
    c->left -= n;
    return p;
}

static unsigned cursor_number(cursor_t *c, unsigned n) {
    const uint8_t *p = cursor_take(c, n);
    unsigned value = 0;
    if (p)
        for (unsigned i = 0; i < n; i++)
            value |= (unsigned)p[i] << (8 * i);
    return value;
}

static unsigned read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

static unsigned read_le24(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16;
}

unsigned desfire_image_size(const uint8_t *p, size_t capacity) {
    if (!p || capacity < IMAGE_MIN_BYTES ||
        (p[0] != UID_SINGLE_BYTES && p[0] != UID_DOUBLE_BYTES && p[0] != UID_TRIPLE_BYTES) ||
        capacity < p[0] + (unsigned)IMAGE_HEADER_BYTES)
        return 0;
    unsigned n = IMAGE_HEADER_BYTES + p[0] + read_be16(p + 1 + p[0]) + read_be16(p + 3 + p[0]);
    if (n > DESFIRE_SLOT_MAX || n > capacity)
        return 0;
    return (n + 3) & ~3u;
}

static bool parse_key_observation(cursor_t *c, uint8_t *state, const uint8_t **settings, uint8_t *length,
                                  const uint8_t **versions, uint8_t *count) {
    *state = cursor_number(c, 1);
    *length = cursor_number(c, 1);
    *settings = cursor_take(c, *length);
    *count = cursor_number(c, 1);
    *versions = cursor_take(c, *count);
    return c->ok && *state <= DESFIRE_CAPTURE_STATE_MAX && *length <= KEY_SETTINGS_BYTES && *count <= KEY_COUNT_MAX &&
           (*state == DESFIRE_CAPTURE_AVAILABLE
                ? (*length == KEY_SETTINGS_BYTES && *count <= ((*settings)[1] & KEY_COUNT_MASK))
                : (!*length && !*count));
}

void desfire_model_reset(desfire_model_t *m) {
    m->selected = -1;
    m->pending = NULL;
    m->pending_len = 0;
    m->version_part = 0;
}

bool desfire_model_load(desfire_model_t *m, const uint8_t *p, size_t size) {
    memset(m, 0, sizeof(*m));
    desfire_model_reset(m);
    unsigned actual = desfire_image_size(p, size);
    if (!actual || actual > size)
        return false;
    m->uid_len = p[0];
    m->uid = p + 1;
    unsigned meta_len = read_be16(p + 1 + m->uid_len), data_len = read_be16(p + 3 + m->uid_len);
    const uint8_t *meta = p + IMAGE_HEADER_BYTES + m->uid_len;
    if (meta_len < CAPTURE_METADATA_MIN_BYTES || meta_len > CAPTURE_METADATA_MAX_BYTES)
        return false;
    bool public_capture = !memcmp(meta, "DFA1", IMAGE_MAGIC_BYTES);
    unsigned start = public_capture ? CAPTURE_HEADER_BYTES : IMAGE_MAGIC_BYTES;
    if ((!public_capture && (memcmp(meta, "DFU1", IMAGE_MAGIC_BYTES) || data_len)) ||
        (public_capture && meta[CAPTURE_VERSION_OFFSET] != CAPTURE_VERSION))
        return false;
    cursor_t c = {meta + start, meta_len - start, true};
    m->atqa = cursor_take(&c, 2);
    m->sak = cursor_number(&c, 1);
    m->ats_len = cursor_number(&c, 1);
    m->ats = cursor_take(&c, m->ats_len);
    if (!c.ok || m->ats_len < DESFIRE_ATS_MIN_BYTES || m->ats_len > DESFIRE_ATS_MAX_BYTES || m->ats[0] != m->ats_len ||
        (m->ats[1] & ATS_RESERVED_FORMAT) || (m->ats[1] & ATS_FRAME_SIZE_MASK) > ATS_FRAME_SIZE_MAX ||
        !(m->sak & DESFIRE_SAK_ISO_DEP) ||
        (m->atqa[0] & ATQA_UID_SIZE_MASK) != (m->uid_len == UID_SINGLE_BYTES   ? 0
                                              : m->uid_len == UID_DOUBLE_BYTES ? ATQA_UID_DOUBLE
                                                                               : ATQA_UID_TRIPLE))
        return false;
    if (!public_capture)
        return c.left == 0;
    /* Public observations follow identity; absence/permission states are
     * validated before any application or file payload can be referenced. */
    m->public_capture = true;
    m->version = cursor_take(&c, VERSION_BYTES);
    unsigned present = cursor_number(&c, 1);
    m->free_memory = cursor_number(&c, 4);
    m->free_present = present == 1;
    if (present > 1 || m->free_memory > FREE_MEMORY_MAX ||
        !parse_key_observation(&c, &m->key_state, &m->keys, &m->key_len, &m->versions, &m->version_len))
        return false;
    m->apps_state = cursor_number(&c, 1);
    if (!c.ok || c.left || m->apps_state > DESFIRE_CAPTURE_STATE_MAX || data_len < CAPTURE_DATA_MIN_BYTES)
        return false;
    const uint8_t *data = meta + meta_len;
    if (memcmp(data, "DFD1", IMAGE_MAGIC_BYTES) || data[CAPTURE_VERSION_OFFSET] != meta[CAPTURE_VERSION_OFFSET])
        return false;
    c = (cursor_t){data + CAPTURE_HEADER_BYTES, data_len - CAPTURE_HEADER_BYTES, true};
    m->app_count = cursor_number(&c, 1);
    if (m->app_count > DESFIRE_APPS_MAX || (m->apps_state && m->app_count))
        return false;
    /* Application AIDs and per-application file IDs must be unique. Every
     * cursor read remains bounded, including unavailable observations. */
    for (unsigned i = 0; i < m->app_count; i++) {
        desfire_app_t *a = &m->apps[i];
        a->aid = cursor_take(&c, DESFIRE_AID_BYTES);
        if (!a->aid || !(a->aid[0] | a->aid[1] | a->aid[2]))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(a->aid, m->apps[j].aid, DESFIRE_AID_BYTES))
                return false;
        if (!parse_key_observation(&c, &a->state, &a->keys, &a->key_len, &a->versions, &a->version_len))
            return false;
        a->files_state = cursor_number(&c, 1);
        a->files = cursor_number(&c, 1);
        a->first_file = m->file_count;
        if (a->files_state > DESFIRE_CAPTURE_STATE_MAX || a->files > FILES_PER_APPLICATION_MAX ||
            a->files + m->file_count > DESFIRE_FILES_MAX || (a->files_state && a->files))
            return false;
        uint32_t ids = 0;
        for (unsigned j = 0; j < a->files; j++) {
            desfire_file_t *f = &m->files[m->file_count++];
            f->id = cursor_number(&c, 1);
            f->settings_state = cursor_number(&c, 1);
            f->settings_len = cursor_number(&c, 2);
            f->settings = cursor_take(&c, f->settings_len);
            f->data_state = cursor_number(&c, 1);
            unsigned n = cursor_number(&c, 4);
            if (n > DESFIRE_SLOT_MAX)
                return false;
            f->data_len = n;
            f->data = cursor_take(&c, n);
            if (!c.ok || f->id > FILE_ID_MAX || (ids & (1u << f->id)) ||
                f->settings_state > DESFIRE_CAPTURE_STATE_MAX || f->data_state > DESFIRE_CAPTURE_STATE_MAX ||
                f->settings_len > CAPTURE_FILE_SETTINGS_MAX_BYTES || (f->data_state && n) ||
                (f->settings_state ? (f->settings_len || !f->data_state)
                                   : f->settings_len < FILE_SETTINGS_COMMON_BYTES))
                return false;
            ids |= 1u << f->id;
            /* Public data must match its declared file geometry and access.
             * Record counts are bounded before multiplying by record width. */
            if (f->data_state == DESFIRE_CAPTURE_AVAILABLE) {
                if (f->settings[1] != DESFIRE_COMMUNICATION_PLAIN ||
                    (f->settings[2] >> 4 != DESFIRE_ACCESS_FREE && f->settings[3] >> 4 != DESFIRE_ACCESS_FREE))
                    return false;
                unsigned type = f->settings[0], expected;
                if (type <= DESFIRE_FILE_BACKUP && f->settings_len >= DATA_FILE_SETTINGS_BYTES)
                    expected = read_le24(f->settings + FILE_CONTENT_SIZE_OFFSET);
                else if (type == DESFIRE_FILE_VALUE && f->settings_len >= VALUE_FILE_SETTINGS_BYTES)
                    expected = VALUE_CONTENT_BYTES;
                else if ((type == DESFIRE_FILE_LINEAR_RECORD || type == DESFIRE_FILE_CYCLIC_RECORD) &&
                         f->settings_len >= RECORD_FILE_SETTINGS_BYTES) {
                    unsigned width = read_le24(f->settings + FILE_CONTENT_SIZE_OFFSET),
                             count = read_le24(f->settings + RECORD_COUNT_OFFSET);
                    if (!width || count > read_le24(f->settings + RECORD_CAPACITY_OFFSET) ||
                        count > DESFIRE_SLOT_MAX / width)
                        return false;
                    expected = count * width;
                } else
                    return false;
                if (expected != n)
                    return false;
            }
        }
    }
    return c.ok && c.left == 0;
}

static unsigned emit_pending_response(desfire_model_t *m, uint8_t *out) {
    unsigned n = m->pending_len > DESFIRE_NATIVE_DATA_MAX ? DESFIRE_NATIVE_DATA_MAX : m->pending_len;
    out[0] = m->pending_len > n ? DESFIRE_STATUS_ADDITIONAL_FRAME : m->pending_status;
    if (n) {
        memcpy(out + 1, m->pending, n);
        m->pending += n;
        m->pending_len -= n;
    }
    return n + 1;
}

static unsigned begin_response(desfire_model_t *m, const uint8_t *data, unsigned n, uint8_t status, uint8_t *out) {
    m->pending = data;
    m->pending_len = n;
    m->pending_status = status;
    return emit_pending_response(m, out);
}

unsigned desfire_model_command(desfire_model_t *m, const uint8_t *p, unsigned size, uint8_t *out) {
    out[0] = DESFIRE_STATUS_ILLEGAL_COMMAND;
    if (!size || !m->public_capture)
        return 1;
    uint8_t command = p[0];
    p++;
    size--;
    if (command == DESFIRE_CMD_ADDITIONAL_FRAME) {
        if (size) {
            out[0] = DESFIRE_STATUS_LENGTH_ERROR;
            return 1;
        }
        if (m->version_part) {
            unsigned part = m->version_part++;
            bool more = part == 1;
            if (!more)
                m->version_part = 0;
            return begin_response(m, m->version + (more ? VERSION_PART_BYTES : VERSION_FINAL_BYTES),
                                  more ? VERSION_PART_BYTES : VERSION_FINAL_BYTES,
                                  more ? DESFIRE_STATUS_ADDITIONAL_FRAME : 0, out);
        }
        return m->pending_len ? emit_pending_response(m, out) : 1;
    }
    m->pending_len = m->version_part = 0;
    desfire_app_t *a = m->selected >= 0 ? &m->apps[m->selected] : NULL;
    switch (command) {
    case DESFIRE_CMD_GET_VERSION:
        if (size)
            break;
        m->version_part = 1;
        return begin_response(m, m->version, VERSION_PART_BYTES, DESFIRE_STATUS_ADDITIONAL_FRAME, out);
    case DESFIRE_CMD_FREE_MEMORY:
        if (size || !m->free_present)
            break;
        for (unsigned i = 0; i < 3; i++)
            m->scratch[i] = m->free_memory >> (i * 8);
        return begin_response(m, m->scratch, 3, 0, out);
    case DESFIRE_CMD_GET_KEY_SETTINGS:
        if (size)
            break;
        if (a ? a->state : m->key_state) {
            out[0] = DESFIRE_STATUS_PERMISSION_DENIED;
            return 1;
        }
        return begin_response(m, a ? a->keys : m->keys, a ? a->key_len : m->key_len, 0, out);
    case DESFIRE_CMD_GET_KEY_VERSION:
        if (size != 1)
            break;
        if (*p >= (a ? a->version_len : m->version_len)) {
            out[0] = DESFIRE_STATUS_PERMISSION_DENIED;
            return 1;
        }
        return begin_response(m, (a ? a->versions : m->versions) + *p, 1, 0, out);
    case DESFIRE_CMD_GET_APPLICATION_IDS:
        if (size)
            break;
        if (m->apps_state) {
            out[0] = DESFIRE_STATUS_PERMISSION_DENIED;
            return 1;
        }
        for (unsigned i = 0; i < m->app_count; i++)
            memcpy(m->scratch + i * DESFIRE_AID_BYTES, m->apps[i].aid, DESFIRE_AID_BYTES);
        return begin_response(m, m->scratch, m->app_count * DESFIRE_AID_BYTES, 0, out);
    case DESFIRE_CMD_SELECT_APPLICATION:
        if (size != DESFIRE_AID_BYTES)
            break;
        if (!(p[0] | p[1] | p[2])) {
            m->selected = -1;
            out[0] = DESFIRE_STATUS_OK;
            return 1;
        }
        for (unsigned i = 0; i < m->app_count; i++)
            if (!memcmp(p, m->apps[i].aid, DESFIRE_AID_BYTES)) {
                m->selected = i;
                out[0] = DESFIRE_STATUS_OK;
                return 1;
            }
        out[0] = DESFIRE_STATUS_APPLICATION_NOT_FOUND;
        return 1;
    case DESFIRE_CMD_GET_FILE_IDS:
        if (size)
            break;
        if (a && a->files_state) {
            out[0] = DESFIRE_STATUS_PERMISSION_DENIED;
            return 1;
        }
        for (unsigned i = 0; a && i < a->files; i++)
            m->scratch[i] = m->files[a->first_file + i].id;
        return begin_response(m, m->scratch, a ? a->files : 0, 0, out);
    case DESFIRE_CMD_GET_FILE_SETTINGS:
    case DESFIRE_CMD_READ_DATA:
    case DESFIRE_CMD_READ_RECORDS:
    case DESFIRE_CMD_GET_VALUE: {
        if (size != ((command == DESFIRE_CMD_READ_DATA || command == DESFIRE_CMD_READ_RECORDS) ? 7 : 1))
            break;
        desfire_file_t *f = NULL;
        for (unsigned i = 0; a && i < a->files; i++)
            if (m->files[a->first_file + i].id == *p)
                f = &m->files[a->first_file + i];
        if (!f) {
            out[0] = DESFIRE_STATUS_FILE_NOT_FOUND;
            return 1;
        }
        if (command == DESFIRE_CMD_GET_FILE_SETTINGS) {
            if (f->settings_state) {
                out[0] = DESFIRE_STATUS_FILE_NOT_FOUND;
                return 1;
            }
            return begin_response(m, f->settings, f->settings_len, 0, out);
        }
        if (f->data_state) {
            out[0] = DESFIRE_STATUS_FILE_NOT_FOUND;
            return 1;
        }
        unsigned offset = 0, n = f->data_len, type = f->settings[0];
        if (command == DESFIRE_CMD_GET_VALUE) {
            if (type != DESFIRE_FILE_VALUE || n != VALUE_CONTENT_BYTES)
                break;
        } else {
            if ((command == DESFIRE_CMD_READ_DATA && type > DESFIRE_FILE_BACKUP) ||
                (command == DESFIRE_CMD_READ_RECORDS && type != DESFIRE_FILE_LINEAR_RECORD &&
                 type != DESFIRE_FILE_CYCLIC_RECORD))
                break;
            unsigned width =
                command == DESFIRE_CMD_READ_RECORDS ? read_le24(f->settings + FILE_CONTENT_SIZE_OFFSET) : 1;
            offset = read_le24(p + 1);
            n = read_le24(p + 4);
            if (!width || offset > f->data_len / width || (n && n > f->data_len / width - offset))
                break;
            offset *= width;
            n = n ? n * width : f->data_len - offset;
        }
        return begin_response(m, f->data + offset, n, 0, out);
    }
    default:
        return 1;
    }
    out[0] = DESFIRE_STATUS_LENGTH_ERROR;
    return 1;
}
