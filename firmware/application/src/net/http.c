#include "http.h"
#include "web_assets.h"
#ifndef WEB_RECOVERY
#include "update_api.h"
#endif
#include <stdio.h>
#include <string.h>
#include <strings.h>

static size_t end_headers(const uint8_t *p, size_t n) {
    for (size_t i = 3; i < n; i++)
        if (!memcmp(p + i - 3, "\r\n\r\n", 4))
            return i + 1;
    return 0;
}
/* 0: absent, 1: present, -1: ambiguous or oversized. */
static int copy_header(const http_request_t *request, http_header_id_t id, char *value,
                       size_t capacity) {
    const http_header_t *header = &request->headers[id];
    if (header->duplicate || header->size >= capacity)
        return -1;
    if (!header->value)
        return 0;
    memcpy(value, header->value, header->size);
    value[header->size] = 0;
    return 1;
}

int http_parse_request(const uint8_t *p, size_t n, http_request_t *request) {
    *request = (http_request_t){.bytes = p, .size = n};
    if (n > HTTP_REQUEST_MAX_BYTES)
        return -1;
    size_t end = end_headers(p, n);
    if (!end)
        return n == HTTP_REQUEST_MAX_BYTES ? -1 : 0;
    request->header_size = end;
    /* Reject malformed names, embedded NUL/control bytes and folded headers. */
    size_t i = 0;
    while (i + 1 < end && p[i] != '\r')
        i++;
    request->line_size = i;
    i += 2;
    static const char *const names[HTTP_HEADER_COUNT] = {
        "Content-Length", "Transfer-Encoding", "Host", "Origin", "Sec-Fetch-Site", "Content-Type"};
    while (i + 2 < end) {
        size_t e = i;
        while (e + 1 < end && (p[e] != '\r' || p[e + 1] != '\n'))
            e++;
        size_t colon = i;
        while (colon < e && p[colon] != ':') {
            uint8_t c = p[colon];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  (c && strchr("!#$%&'*+-.^_`|~", c))))
                return -1;
            colon++;
        }
        if (colon == i || colon == e)
            return -1;
        for (size_t a = colon + 1; a < e; a++)
            if ((p[a] < 32 && p[a] != '\t') || p[a] == 127)
                return -1;
        size_t a = colon + 1, b = e;
        while (a < b && (p[a] == ' ' || p[a] == '\t'))
            a++;
        while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t'))
            b--;
        for (unsigned id = 0; id < HTTP_HEADER_COUNT; id++)
            if (colon - i == strlen(names[id]) &&
                !strncasecmp((const char *)p + i, names[id], colon - i)) {
                http_header_t *header = &request->headers[id];
                header->duplicate = header->value != NULL;
                header->value = p + a;
                header->size = b - a;
                break;
            }
        i = e + 2;
    }
    char value[64];
    if (copy_header(request, HTTP_TRANSFER_ENCODING, value, sizeof(value)) != 0)
        return -1;
    size_t body = 0;
    int length = copy_header(request, HTTP_CONTENT_LENGTH, value, sizeof(value));
    if (length < 0)
        return -1;
    if (length == 1) {
        if (!*value)
            return -1;
        for (char *v = value; *v; v++) {
            if (*v < '0' || *v > '9' || body > HTTP_REQUEST_MAX_BYTES / 10)
                return -1;
            body = body * 10 + *v - '0';
        }
        if (body > 1024u)
            return -1;
    }
    size_t total = end + body;
    if (total > HTTP_REQUEST_MAX_BYTES || n > total)
        return -1;
    request->complete = n == total;
    return request->complete ? 1 : 0;
}

http_response_t http_dispatch_request(const http_request_t *request, uint8_t *out) {
    http_response_t r = {.status = 400, .body = out, .size = 0};
    const uint8_t *p = request->bytes;
    size_t n = request->size, end = request->header_size;
    char method[8], uri[512], version[16], host[64], origin[96], fetch[32];
    if (!request->complete)
        goto done;
    size_t line = request->line_size;
    if (line >= 600)
        goto done;
    char first[600];
    memcpy(first, p, line);
    first[line] = 0;
    char extra;
    if (sscanf(first, "%7s %511s %15s %c", method, uri, version, &extra) != 3 ||
        strcmp(version, "HTTP/1.1"))
        goto done;
    if (copy_header(request, HTTP_HOST, host, sizeof(host)) != 1 ||
        (strcmp(host, "192.168.16.1") && strcmp(host, "192.168.16.1:80")
#ifdef PLATFORM_HOST
         && strcmp(host, "127.0.0.1:8080") && strcmp(host, "localhost:8080")
#endif
             )) {
        r.status = 403;
        goto done;
    }
    if (!strcmp(method, "GET")) {
        const web_asset_t *asset = web_asset_find(!strcmp(uri, "/update") ? "/update.html" : uri);
        if (asset) {
            r.status = 200; r.body = asset->bytes; r.size = asset->size;
            r.header_size = snprintf(r.header, sizeof(r.header),
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Encoding: gzip\r\n", asset->type);
            goto done;
        }
    }
    if (!strcmp(method, "POST")) {
        int present = copy_header(request, HTTP_ORIGIN, origin, sizeof(origin));
        if (present < 0)
            goto done;
        if (present == 1) {
            char allowed[80];
            snprintf(allowed, sizeof(allowed), "http://%s", host);
            if (strcmp(origin, allowed)) {
                r.status = 403;
                goto done;
            }
        }
        present = copy_header(request, HTTP_FETCH_SITE, fetch, sizeof(fetch));
        if (present < 0)
            goto done;
        if (present == 1 && strcmp(fetch, "same-origin") && strcmp(fetch, "none")) {
            r.status = 403;
            goto done;
        }
        char content[64];
        if (n > end && (copy_header(request, HTTP_CONTENT_TYPE, content, sizeof(content)) != 1 ||
                        strcmp(content, "application/octet-stream"))) {
            r.status = 415;
            goto done;
        }
    }
    api_response_t api;
#ifndef WEB_RECOVERY
    if (!strncmp(uri, "/api/update", 11))
        api = web_update_application_request(method, uri, p + end, n - end, out, HTTP_RESPONSE_MAX_BYTES);
    else
#endif
        api = api_dispatch_request(method, uri, p + end, n - end, out, HTTP_RESPONSE_MAX_BYTES);
    r.status = api.status;
    r.size = api.size;
    r.header_size = snprintf(r.header, sizeof(r.header),
                             "HTTP/1.1 %u Response\r\nContent-Type: %s\r\n", r.status, api.type);
done:
    if (!r.header_size)
        r.header_size = snprintf(
            r.header, sizeof(r.header),
            "HTTP/1.1 %u Response\r\nContent-Type: text/html; charset=utf-8\r\n%s", r.status,
            "");
    r.header_size += snprintf(
        r.header + r.header_size, sizeof(r.header) - r.header_size,
        "Content-Length: %u\r\nConnection: close\r\nCache-Control: "
        "no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src "
        "'self'; connect-src " WEB_RESOURCE_CONNECT "; worker-src 'self'; script-src 'self' "
        "'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; font-src "
        "data:; object-src "
        "'none'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n",
        (unsigned)r.size);
    return r;
}
