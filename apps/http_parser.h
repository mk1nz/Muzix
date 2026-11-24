#ifndef MUZIX_HTTP_PARSER_H
#define MUZIX_HTTP_PARSER_H

#include "ztypes.h"

#define HTTP_URI_MAX 128
#define HTTP_UA_MAX 256
#define HTTP_HEADER_TOTAL_MAX 4096
#define HTTP_MAX_HEADERS 64
#define CL_BUF_MAX 11

typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET = 1,
    HTTP_METHOD_HEAD = 2,
    HTTP_METHOD_POST = 3
} http_method_t;

typedef enum {
    HTTP_VERSION_UNKNOWN = 0,
    HTTP_VERSION_1_0 = 1,
    HTTP_VERSION_1_1 = 2
} http_version_t;

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_COMPLETE = 1,
    HTTP_PARSE_ERROR_400 = -1,
    HTTP_PARSE_ERROR_414 = -2,
    HTTP_PARSE_ERROR_431 = -3,
    HTTP_PARSE_ERROR_501 = -4
} http_parse_result_t;

typedef enum {
    STATE_METHOD = 0,
    STATE_URI = 1,
    STATE_URI_DROP_QUERY = 2,
    STATE_VERSION = 3,
    STATE_HEADER_NAME = 4,
    STATE_HEADER_VALUE = 5,
    STATE_HEADER_SKIP_VALUE = 6,
    STATE_LF = 7,
    STATE_LF2 = 8,
    STATE_COMPLETE = 9,
    STATE_ERROR = 10
} parser_state_t;

/* Header type IDs - only for headers we need to recognize for special handling */
typedef enum {
    HDR_NONE = 0,
    HDR_USER_AGENT = 1,
    HDR_CONTENT_LENGTH = 2,
    HDR_TRANSFER_ENCODING = 3,
} header_type_t;

/* Flag bit definitions */
#define FLAG_METHOD_MASK     0x03  /* bits 0-1: method (0-3) */
#define FLAG_METHOD_SHIFT    0
#define FLAG_VERSION_MASK    0x0C  /* bits 2-3: version (0-2) */
#define FLAG_VERSION_SHIFT   2
#define FLAG_CL_SET          0x10  /* bit 4: content_length_set */
#define FLAG_UA_PARSED       0x20  /* bit 5: user_agent parsed and stored */
#define FLAG_HDR_RELEVANT    0x80  /* bit 7: header is relevant for value storage */

#define GET_METHOD(f)        ((f) & 0x03)
#define SET_METHOD(f, v)     ((f) = ((f) & ~0x03) | ((v) & 0x03))
#define GET_VERSION(f)       (((f) >> 2) & 0x03)
#define SET_VERSION(f, v)    ((f) = ((f) & ~0x0C) | (((v) & 0x03) << 2))
#define GET_CL_SET(f)        (((f) >> 4) & 0x01)
#define SET_CL_SET(f, v)     ((f) = ((f) & ~0x10) | (((v) & 0x01) << 4))
#define GET_UA_PARSED(f)     (((f) >> 5) & 0x01)
#define SET_UA_PARSED(f, v)  ((f) = ((f) & ~0x20) | (((v) & 0x01) << 5))
#define IS_HDR_RELEVANT(f)   ((f) & 0x80)
#define SET_HDR_RELEVANT(f)  ((f) |= 0x80)
#define CLR_HDR_RELEVANT(f)  ((f) &= ~0x80)

struct http_parser {
    /* State machine and flags (packed into bytes) */
    uint8_t state;                 /* parser_state_t */
    uint8_t flags;                 /* method, version, cl_set, ua_parsed, hdr_relevant */
    uint8_t crlf_state;            /* CRLF parsing state */
    uint8_t method_pos;            /* position in method parsing */
    uint8_t version_pos;           /* position in version parsing */
    uint8_t uri_len;               /* URI length */
    uint8_t header_name_len;       /* current header name length (for counting) */
    uint8_t header_name_first;     /* first char of header name (lowercase) */
    uint8_t header_name_last;      /* last char of header name (lowercase) */
    uint8_t header_value_len;      /* current header value length (for counting) */
    uint16_t ua_len;               /* User-Agent length */
    uint8_t cl_buf_len;            /* Content-Length buffer length */
    bool cl_error;                 /* Content-Length parse error */
    uint8_t te_match_pos;          /* position in "chunked" matching */
    bool te_chunked_found;         /* "chunked" found in value */
    uint16_t total_header_bytes;   /* total header section bytes */
    uint8_t header_count;          /* number of headers parsed */
    int16_t error_code;            /* error code if any */

    /* Current header being parsed */
    header_type_t current_header;  /* identified header type */

    /* Parsed values */
    uint32_t content_length;

    /* Buffers (only for data we actually need to keep) */
    char uri[HTTP_URI_MAX + 1];
    char user_agent[HTTP_UA_MAX + 1];
    char cl_buf[CL_BUF_MAX];       /* Content-Length value buffer */
};

typedef struct http_parser http_parser_t;

void http_parser_init(http_parser_t *parser);
http_parse_result_t http_parser_feed(http_parser_t *parser, const uint8_t *data, uint16_t len);
void http_parser_reset(http_parser_t *parser);

http_method_t http_parser_method(const http_parser_t *parser);
const char *http_parser_uri(const http_parser_t *parser);
http_version_t http_parser_version(const http_parser_t *parser);
const char *http_parser_user_agent(const http_parser_t *parser);
uint32_t http_parser_content_length(const http_parser_t *parser);
int http_parser_error_code(const http_parser_t *parser);

#endif