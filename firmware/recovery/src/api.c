#include "net/api.h"
#include "web_update.h"
#include "recovery.h"
#include <stdio.h>
#include <string.h>

static const char write_route[] = "/api/update/write?session=";
static const char system_route[] = "/api/update/system?session=";
static const char commit_route[] = "/api/update/commit?session=";
static const char offset_query[] = "&offset=";

static bool number(const char **p, uint32_t *value) {
    *value = 0;
    if (**p < '0' || **p > '9') return false;
    while (**p >= '0' && **p <= '9') {
        unsigned digit = *(*p)++ - '0';
        if (*value > (UINT32_MAX - digit) / 10) return false;
        *value = *value * 10 + digit;
    }
    return true;
}

api_response_t api_dispatch_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *out, size_t capacity) {
    unsigned status = 404;
    if (!strcmp(method, "GET") && !strcmp(uri, "/api/update")) {
        bool valid = web_update_cache_valid();
        uint32_t version = valid ? web_update_version() : 0;
        int length = snprintf((char *)out, capacity,
            "{\"mode\":\"recovery\",\"layout\":%u,\"maximum\":%u,\"valid\":%s,"
            "\"version\":%lu,\"session\":%lu,\"received\":%lu,\"size\":%lu,\"recovery_crc32\":%lu}",
            web_update_bootloader_ready() ? WEB_UPDATE_LAYOUT : 0,
            WEB_UPDATE_MAX, valid ? "true" : "false",
            (unsigned long)version, (unsigned long)web_update_session(),
            (unsigned long)web_update_received(), (unsigned long)web_update_size(),
            (unsigned long)web_update_u32(web_update_flash(WEB_RECOVERY_CRC)));
        return (api_response_t){length > 0 && (size_t)length < capacity ? 200 : 500,
                                "application/json", length > 0 && (size_t)length < capacity ? (size_t)length : 0};
    }
    if (!strcmp(method, "POST")) {
        if (!web_update_bootloader_ready())
            return (api_response_t){409, "application/octet-stream", 0};
        if (!strcmp(uri, "/api/update/begin")) status = web_update_begin(body, size);
        else if (!strncmp(uri, write_route, sizeof(write_route) - 1) ||
                 !strncmp(uri, system_route, sizeof(system_route) - 1)) {
            bool system = !strncmp(uri, system_route, sizeof(system_route) - 1);
            const char *p = uri + (system ? sizeof(system_route) - 1 : sizeof(write_route) - 1);
            uint32_t id, offset;
            if (!number(&p, &id) || strncmp(p, offset_query, sizeof(offset_query) - 1)) status = 400;
            else { p += sizeof(offset_query) - 1; status = number(&p, &offset) && !*p ?
                (system ? web_update_system_write(id, offset, body, size) : web_update_write(id, offset, body, size)) : 400; }
        } else if (!strncmp(uri, commit_route, sizeof(commit_route) - 1)) {
            const char *p = uri + sizeof(commit_route) - 1;
            uint32_t id;
            status = !size && number(&p, &id) && !*p ? web_update_commit(id) : 400;
        } else if (!strcmp(uri, "/api/update/reboot")) {
            status = !size && !web_update_active() && (web_update_restoring() || web_update_application_valid()) ? 200 : 409;
            if (status == 200) recovery_reboot_later();
        }
    }
    return (api_response_t){status, "application/octet-stream", 0};
}

void web_session_end(void) {} /* USB reconnects never invalidate accepted flash bytes. */
void web_session_disconnect(api_source_t source) { (void)source; }
void web_session_poll(void) {}
bool web_session_active(void) { return web_update_active(); }
