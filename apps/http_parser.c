#include "http_parser.h"
#include "libc.h"

/* Case-insensitive character comparison */
static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

/* Buffer for Content-Length value (max 10 digits + null) */
#define CL_BUF_MAX 11

/* Which headers need value storage/parsing */
static inline bool hdr_needs_value_storage(header_type_t hdr) {
    return (hdr == HDR_USER_AGENT) || (hdr == HDR_CONTENT_LENGTH) || (hdr == HDR_TRANSFER_ENCODING);
}

/* Identify header type from first/last char and length - using jump table approach */
static header_type_t identify_header_fast(uint8_t first, uint8_t last, uint8_t len) {
    if (len == 10 && first == 'u' && last == 't') return HDR_USER_AGENT;
    if (len == 14 && first == 'c' && last == 'h') return HDR_CONTENT_LENGTH;
    if (len == 17 && first == 't' && last == 'g') return HDR_TRANSFER_ENCODING;
    return HDR_NONE;
}

/* Parse Content-Length from buffer using shifts (math library already linked by httpd.c).
 * Returns 1 on success, -1 on error/overflow. */
static int parse_cl_buffer(const char *buf, uint8_t len, uint32_t *out) {
    uint32_t result = 0;
    for (uint8_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c < '0' || c > '9') return -1;
        uint32_t digit = c - '0';
        if (result > 429496729u || (result == 429496729u && digit > 5u)) return -1;
        result = (result << 3) + (result << 1) + digit;
    }
    *out = result;
    return 1;
}

/* Check for "chunked" in buffer - simplified state machine */
static bool check_chunked_buffer(const char *buf, uint8_t len) {
    static const char needle[] = "chunked";
    if (len < 7) return false;
    for (uint8_t i = 0; i <= len - 7; i++) {
        uint8_t match = 1;
        for (uint8_t j = 0; j < 7; j++) {
            char c1 = buf[i + j];
            char c2 = needle[j];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) { match = 0; break; }
        }
        if (match) return true;
    }
    return false;
}

static void parser_reset_fields(http_parser_t *p) {
    p->state = STATE_METHOD;
    p->flags = 0;
    p->crlf_state = 0;
    p->method_pos = 0;
    p->version_pos = 0;
    p->uri_len = 0;
    p->header_name_len = 0;
    p->header_name_first = 0;
    p->header_name_last = 0;
    p->header_value_len = 0;
    p->ua_len = 0;
    p->total_header_bytes = 0;
    p->header_count = 0;
    p->error_code = 0;
    p->content_length = 0;
    p->current_header = HDR_NONE;
    p->uri[0] = '\0';
    p->user_agent[0] = '\0';
}

void http_parser_init(http_parser_t *parser) {
    if (!parser) return;
    uint8_t *p = (uint8_t *)parser;
    for (uint16_t i = 0; i < sizeof(http_parser_t); i++) {
        p[i] = 0;
    }
    parser_reset_fields(parser);
}

void http_parser_reset(http_parser_t *parser) {
    if (!parser) return;
    parser_reset_fields(parser);
}

http_parse_result_t http_parser_feed(http_parser_t *parser, const uint8_t *data, uint16_t len) {
    if (!parser || !data) return HTTP_PARSE_ERROR_400;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (parser->state >= STATE_HEADER_NAME && parser->state <= STATE_HEADER_SKIP_VALUE) {
            if (parser->total_header_bytes >= HTTP_HEADER_TOTAL_MAX) {
                parser->state = STATE_ERROR;
                parser->error_code = 431;
                return HTTP_PARSE_ERROR_431;
            }
            parser->total_header_bytes++;
        }

        switch (parser->state) {
            case STATE_METHOD: {
                if (c == ' ') {
                    if (parser->method_pos == 3) {
                        SET_METHOD(parser->flags, HTTP_METHOD_GET);
                    } else if (parser->method_pos == 4) {
                        SET_METHOD(parser->flags, parser->version_pos == 'H' ? HTTP_METHOD_HEAD : HTTP_METHOD_POST);
                    } else {
                        parser->state = STATE_ERROR;
                        parser->error_code = 501;
                        return HTTP_PARSE_ERROR_501;
                    }
                    if (GET_METHOD(parser->flags) == HTTP_METHOD_UNKNOWN) {
                        parser->state = STATE_ERROR;
                        parser->error_code = 501;
                        return HTTP_PARSE_ERROR_501;
                    }
                    parser->state = STATE_URI;
                    parser->uri_len = 0;
                } else if (parser->method_pos < 4) {
                    if (parser->method_pos == 0) {
                        if (c == 'G' || c == 'H' || c == 'P') parser->version_pos = c;
                        else { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; }
                    } else if (parser->method_pos == 1) {
                        if ((parser->version_pos == 'G' && c != 'E') || (parser->version_pos == 'H' && c != 'E') || (parser->version_pos == 'P' && c != 'O')) {
                            parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501;
                        }
                    } else if (parser->method_pos == 2) {
                        if ((parser->version_pos == 'G' && c != 'T') || (parser->version_pos == 'H' && c != 'A') || (parser->version_pos == 'P' && c != 'S')) {
                            parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501;
                        }
                    } else if (parser->method_pos == 3) {
                        if (parser->version_pos == 'G') { if (c != ' ') { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; } }
                        else if (parser->version_pos == 'H') { if (c != 'D') { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; } }
                        else if (parser->version_pos == 'P') { if (c != 'T') { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; } }
                    }
                    parser->method_pos++;
                } else { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; }
                break;
            }

            case STATE_URI: {
                if (c == ' ') {
                    parser->uri[parser->uri_len] = '\0';
                    if (parser->uri_len > HTTP_URI_MAX) { parser->state = STATE_ERROR; parser->error_code = 414; return HTTP_PARSE_ERROR_414; }
                    parser->state = STATE_VERSION;
                    parser->version_pos = 0;
                } else if (c == '?') {
                    parser->uri[parser->uri_len] = '\0';
                    parser->state = STATE_URI_DROP_QUERY;
                } else if (parser->uri_len < HTTP_URI_MAX) {
                    parser->uri[parser->uri_len++] = c;
                } else { parser->state = STATE_ERROR; parser->error_code = 414; return HTTP_PARSE_ERROR_414; }
                break;
            }

            case STATE_URI_DROP_QUERY: {
                if (c == ' ') { parser->state = STATE_VERSION; parser->version_pos = 0; }
                break;
            }

            case STATE_VERSION: {
                if (c == '\r') {
                    if (parser->version_pos == 8) {
                        static const char v10[8] = {'H','T','T','P','/','1','.','0'};
                        static const char v11[8] = {'H','T','T','P','/','1','.','1'};
                        bool m10 = true, m11 = true;
                        for (int j = 0; j < 8; j++) {
                            if (parser->user_agent[j] != v10[j]) m10 = false;
                            if (parser->user_agent[j] != v11[j]) m11 = false;
                        }
                        if (m10) SET_VERSION(parser->flags, HTTP_VERSION_1_0);
                        else if (m11) SET_VERSION(parser->flags, HTTP_VERSION_1_1);
                    }
                    parser->user_agent[0] = '\0';
                    parser->ua_len = 0;
                    if (GET_VERSION(parser->flags) == HTTP_VERSION_UNKNOWN) { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                    parser->state = STATE_LF;
                    parser->crlf_state = 1;
                } else if (parser->version_pos < 8) {
                    if (parser->version_pos < 7) {
                        static const char vp[7] = {'H','T','T','P','/','1','.'};
                        if (c != vp[parser->version_pos]) { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                    } else if (parser->version_pos == 7) {
                        if (c == '0') SET_VERSION(parser->flags, HTTP_VERSION_1_0);
                        else if (c == '1') SET_VERSION(parser->flags, HTTP_VERSION_1_1);
                        else { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                    }
                    parser->user_agent[parser->version_pos] = c;
                    parser->version_pos++;
                } else { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                break;
            }

            case STATE_HEADER_NAME: {
                if (c == ':') {
                    parser->current_header = identify_header_fast(parser->header_name_first, parser->header_name_last, parser->header_name_len);
                    parser->header_count++;
                    if (parser->header_count > HTTP_MAX_HEADERS) { parser->state = STATE_ERROR; parser->error_code = 431; return HTTP_PARSE_ERROR_431; }
                    if (parser->current_header != HDR_NONE && hdr_needs_value_storage(parser->current_header)) {
                        SET_HDR_RELEVANT(parser->flags);
                        parser->state = STATE_HEADER_VALUE;
                        parser->header_value_len = 0;
                        parser->ua_len = 0;
                    } else {
                        CLR_HDR_RELEVANT(parser->flags);
                        parser->state = STATE_HEADER_SKIP_VALUE;
                    }
                    parser->header_name_len = 0;
                } else if (c == '\r') {
                    if (parser->header_name_len == 0) {
                        parser->state = STATE_LF2;
                        parser->crlf_state = 3;
                    } else { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                } else {
                    char lc = to_lower(c);
                    if (parser->header_name_len == 0) parser->header_name_first = lc;
                    parser->header_name_last = lc;
                    parser->header_name_len++;
                    /* No need to buffer header name in user_agent - identify_header_fast uses first/last/len */
                }
                break;
            }

            case STATE_HEADER_SKIP_VALUE: {
                if (c == '\r') { parser->state = STATE_LF; parser->crlf_state = 1; }
                break;
            }

            case STATE_HEADER_VALUE: {
                if (c == '\r') {
                    header_type_t hdr = parser->current_header;
                    if (hdr == HDR_CONTENT_LENGTH) {
                        if (GET_CL_SET(parser->flags)) { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                        if (parse_cl_buffer(parser->user_agent, parser->ua_len, &parser->content_length) != 1) {
                            parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400;
                        }
                        SET_CL_SET(parser->flags, 1);
                    } else if (hdr == HDR_TRANSFER_ENCODING) {
                        if (check_chunked_buffer(parser->user_agent, parser->ua_len)) { parser->state = STATE_ERROR; parser->error_code = 501; return HTTP_PARSE_ERROR_501; }
                    } else if (hdr == HDR_USER_AGENT) {
                        parser->user_agent[parser->ua_len] = '\0';
                        SET_UA_PARSED(parser->flags, 1);
                        /* Move UA to offset 17 to preserve during subsequent header name buffering */
                        if (parser->ua_len <= 17) {
                            for (int j = parser->ua_len - 1; j >= 0; j--) parser->user_agent[j + 17] = parser->user_agent[j];
                            parser->user_agent[parser->ua_len + 17] = '\0';
                        }
                        /* Don't reset ua_len - keep it to know UA was parsed and its length */
                    } else {
                        parser->ua_len = 0;
                    }
                    parser->header_value_len = 0;
                    /* Only reset ua_len for non-UA headers */
                    if (parser->current_header != HDR_USER_AGENT) {
                        parser->ua_len = 0;
                    }
                    CLR_HDR_RELEVANT(parser->flags);
                    parser->state = STATE_LF;
                    parser->crlf_state = 1;
                } else if (c == ' ' && parser->header_value_len == 0) {
                    /* Skip leading whitespace */
                } else {
                    header_type_t hdr = parser->current_header;
                    if (hdr == HDR_USER_AGENT) {
                        if (parser->ua_len < HTTP_UA_MAX) parser->user_agent[parser->ua_len++] = c;
                        else { parser->state = STATE_ERROR; parser->error_code = 431; return HTTP_PARSE_ERROR_431; }
                    } else if (hdr == HDR_CONTENT_LENGTH || hdr == HDR_TRANSFER_ENCODING) {
                        if (parser->ua_len < HTTP_UA_MAX) parser->user_agent[parser->ua_len++] = c;
                    }
                    parser->header_value_len++;
                }
                break;
            }

            case STATE_LF: {
                if (c == '\r') { parser->state = STATE_LF2; parser->crlf_state = 3; }
                else if (c == '\n') { parser->state = STATE_HEADER_NAME; parser->header_name_len = 0; parser->crlf_state = 0; }
                else { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                break;
            }

            case STATE_LF2: {
                if (c == '\n') { parser->crlf_state = 4; parser->state = STATE_COMPLETE; return HTTP_PARSE_COMPLETE; }
                else { parser->state = STATE_ERROR; parser->error_code = 400; return HTTP_PARSE_ERROR_400; }
                break;
            }

            case STATE_COMPLETE:
                return HTTP_PARSE_COMPLETE;

            case STATE_ERROR:
                if (parser->error_code == 414) return HTTP_PARSE_ERROR_414;
                if (parser->error_code == 431) return HTTP_PARSE_ERROR_431;
                if (parser->error_code == 501) return HTTP_PARSE_ERROR_501;
                return HTTP_PARSE_ERROR_400;

            default:
                parser->state = STATE_ERROR;
                parser->error_code = 400;
                return HTTP_PARSE_ERROR_400;
        }
    }

    if (parser->state == STATE_COMPLETE) return HTTP_PARSE_COMPLETE;
    return HTTP_PARSE_OK;
}

http_method_t http_parser_method(const http_parser_t *parser) {
    return parser ? GET_METHOD(parser->flags) : HTTP_METHOD_UNKNOWN;
}

const char *http_parser_uri(const http_parser_t *parser) {
    return parser ? parser->uri : "";
}

http_version_t http_parser_version(const http_parser_t *parser) {
    return parser ? GET_VERSION(parser->flags) : HTTP_VERSION_UNKNOWN;
}

const char *http_parser_user_agent(const http_parser_t *parser) {
    if (!parser) return "";
    if (GET_UA_PARSED(parser->flags) && parser->ua_len <= 17) return parser->user_agent + 17;
    return parser->user_agent;
}

uint32_t http_parser_content_length(const http_parser_t *parser) {
    return parser ? parser->content_length : 0;
}

int http_parser_error_code(const http_parser_t *parser) {
    return parser ? parser->error_code : 0;
}