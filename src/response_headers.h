#ifndef CAPSID_RESPONSE_HEADERS_H
#define CAPSID_RESPONSE_HEADERS_H

#include <stddef.h>
#include <stdint.h>

namespace capsid {

struct ResponseHeaderView {
    ResponseHeaderView()
        : name(NULL), name_size(0), value(NULL), value_size(0) {}

    const uint8_t *name;
    size_t name_size;
    const uint8_t *value;
    size_t value_size;
};

bool decode_response_headers(const uint8_t *data,
                             size_t size,
                             size_t wanted_index,
                             size_t *out_count,
                             ResponseHeaderView *out_header);

}  // namespace capsid

#endif
