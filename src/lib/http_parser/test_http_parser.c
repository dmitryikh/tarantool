#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include "http_parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        struct http_parser parser;
        char *buf = (char*)data;
        http_parser_create(&parser);
        parser.hdr_name = (char *)calloc((int)size, sizeof(char));
        if (parser.hdr_name == NULL)
                return -1;
        char *end_buf = buf + size;
        int rc = http_parse_header_line(&parser, &buf, end_buf, size);
        free(parser.hdr_name);

        return rc;
}
