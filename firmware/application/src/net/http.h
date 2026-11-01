#pragma once
#include "net/api.h"
#define HTTP_REQUEST_MAX_BYTES 2048u
#define HTTP_RESPONSE_MAX_BYTES 512u
typedef enum {
    HTTP_CONTENT_LENGTH,
    HTTP_TRANSFER_ENCODING,
    HTTP_HOST,
    HTTP_ORIGIN,
    HTTP_FETCH_SITE,
    HTTP_CONTENT_TYPE,
    HTTP_HEADER_COUNT
} http_header_id_t;
typedef struct {
    const uint8_t *value;
    size_t size;
    bool duplicate;
} http_header_t;
typedef struct {
    const uint8_t *bytes;
    size_t size, line_size, header_size;
    http_header_t headers[HTTP_HEADER_COUNT];
    bool complete;
} http_request_t;
typedef struct {
    unsigned status;
    const uint8_t *body;
    size_t size;
    char header[512];
    size_t header_size;
} http_response_t;
/* 0: incomplete; -1: rejected; 1: complete. The parsed view borrows request bytes.
 * No allocation or flash mutation; incomplete/rejected views dispatch as HTTP 400. */
int http_parse_request(const uint8_t *bytes, size_t received, http_request_t *request);
http_response_t http_dispatch_request(const http_request_t *request, uint8_t *output);
