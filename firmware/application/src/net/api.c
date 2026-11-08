#include "api.h"
#include "tag_emulation.h"
#include <string.h>

static bool number(const char **text, uint32_t *value) {
    const char *p = *text;
    if (*p < '0' || *p > '9') return false;
    uint32_t n = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned digit = *p - '0';
        if (n > (UINT32_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *text = p;
    *value = n;
    return true;
}

static api_operation_t route(const char *method, const char *uri, uint32_t *id, uint16_t *argument) {
    if (!strcmp(method, "GET")) {
        if (!strcmp(uri, "/api/device")) return API_DEVICE;
        if (!strcmp(uri, "/api/status")) return API_STATUS;
        if (!strcmp(uri, "/api/update")) return API_UPDATE_INFO;
        if (!strncmp(uri, "/api/slot/", 10) && strlen(uri) == 11 && uri[10] >= '0' && uri[10] <= '7') {
            *argument = uri[10] - '0'; return API_SLOT;
        }
        return API_UNKNOWN_READ;
    }
    if (strcmp(method, "POST")) return API_UNKNOWN_WRITE;
    if (!strcmp(uri, "/api/session")) return API_READER_SESSION;
    if (!strcmp(uri, "/api/slots/session")) return API_SLOT_SESSION;
    if (!strcmp(uri, "/api/settings")) return API_SETTINGS;
    static const struct { const char *path; api_operation_t operation; } routes[] = {
        {"/api/keepalive?id=", API_KEEPALIVE},
        {"/api/end?id=", API_END},
        {"/api/classic/job/start?id=", API_CLASSIC_JOB_START},
        {"/api/classic/job/cancel?id=", API_CLASSIC_JOB_CANCEL},
        {"/api/classic/job/read?id=", API_CLASSIC_JOB_READ},
        {"/api/st25tb/tearoff/start?id=", API_TEAROFF_START},
        {"/api/st25tb/tearoff/state?id=", API_TEAROFF_STATE},
        {"/api/st25tb/tearoff/cancel?id=", API_TEAROFF_CANCEL},
        {"/api/slots/open?id=", API_SLOT_OPEN},
        {"/api/slots/read?id=", API_SLOT_READ},
        {"/api/slots/begin?id=", API_SLOT_BEGIN},
        {"/api/slots/write?id=", API_SLOT_WRITE},
        {"/api/slots/commit?id=", API_SLOT_COMMIT},
        {"/api/slots/activate?id=", API_SLOT_ACTIVATE},
        {"/api/slots/nick?id=", API_SLOT_NICK},
        {"/api/slots/enable?id=", API_SLOT_ENABLE},
        {"/api/slots/clear?id=", API_SLOT_CLEAR},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        size_t length = strlen(routes[i].path);
        if (strncmp(uri, routes[i].path, length)) continue;
        const char *p = uri + length;
        return number(&p, id) && !*p ? routes[i].operation : API_UNKNOWN_WRITE;
    }
    if (!strncmp(uri, "/api/command/", 13)) {
        const char *p = uri + 13;
        uint32_t command;
        if (number(&p, &command) && command <= UINT16_MAX && !strncmp(p, "?id=", 4)) {
            p += 4;
            if (number(&p, id) && !*p) { *argument = command; return API_COMMAND; }
        }
    }
    return API_UNKNOWN_WRITE;
}

api_response_t api_dispatch_request(const char *method, const char *uri,
    const uint8_t *body, size_t size, uint8_t *output, size_t capacity) {
    // Metadata polling can continue while the physical shortcut is displayed.
    if (strcmp(method, "GET")) tag_emulation_web_link_stop();
    uint32_t id = 0;
    uint16_t argument = 0;
    api_operation_t operation = route(method, uri, &id, &argument);
    return api_execute(API_SOURCE_HTTP, operation, id, argument, body, size, output, capacity);
}
