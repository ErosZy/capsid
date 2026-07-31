#include "response_headers.h"

#include "protocol.h"

namespace capsid {

bool decode_response_headers(const uint8_t *data,
                             size_t size,
                             size_t wanted_index,
                             size_t *out_count,
                             ResponseHeaderView *out_header) {
    if (out_header) {
        *out_header = ResponseHeaderView();
    }
    if (!data || size < sizeof(uint16_t) ||
        size > protocol::kMaxPayloadSize) {
        return false;
    }

    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    uint16_t count = 0;
    if (!protocol::read_u16(&cursor, end, &count)) {
        return false;
    }

    ResponseHeaderView candidate;
    for (size_t index = 0; index < count; ++index) {
        uint16_t name_size = 0;
        uint32_t value_size = 0;
        if (!protocol::read_u16(&cursor, end, &name_size) ||
            static_cast<size_t>(end - cursor) < name_size) {
            return false;
        }
        const uint8_t *name = cursor;
        cursor += name_size;
        if (!protocol::read_u32(&cursor, end, &value_size) ||
            static_cast<uint64_t>(end - cursor) < value_size) {
            return false;
        }
        const uint8_t *value = cursor;
        cursor += value_size;
        if (index == wanted_index) {
            candidate.name = name;
            candidate.name_size = name_size;
            candidate.value = value;
            candidate.value_size = value_size;
        }
    }
    if (cursor != end ||
        (out_header && wanted_index >= static_cast<size_t>(count))) {
        return false;
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_header) {
        *out_header = candidate;
    }
    return true;
}

}  // namespace capsid
