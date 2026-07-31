#include "response_headers.h"
#include "protocol.h"

#include <stdint.h>

#include <cstdlib>
#include <limits>
#include <vector>

namespace {

void exercise(const uint8_t *data, size_t size, size_t wanted) {
    size_t count = std::numeric_limits<size_t>::max();
    capsid::ResponseHeaderView header;
    const bool valid = capsid::decode_response_headers(
        data, size, wanted, &count, &header);
    if (!valid) {
        if (header.name != NULL || header.name_size != 0 ||
            header.value != NULL || header.value_size != 0) {
            std::abort();
        }
        return;
    }

    size_t verified_count = 0;
    if (!capsid::decode_response_headers(
            data, size, 0, &verified_count, NULL) ||
        verified_count != count) {
        std::abort();
    }
    if (count == 0) {
        return;
    }

    const size_t selected = wanted % count;
    if (!capsid::decode_response_headers(
            data, size, selected, NULL, &header)) {
        std::abort();
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    const uintptr_t end = begin + size;
    const uintptr_t name =
        reinterpret_cast<uintptr_t>(header.name);
    const uintptr_t value =
        reinterpret_cast<uintptr_t>(header.value);
    if (end < begin || name < begin || name > end ||
        header.name_size > end - name ||
        value < begin || value > end ||
        header.value_size > end - value) {
        std::abort();
    }
}

void exercise_structured(const uint8_t *data, size_t size) {
    std::vector<uint8_t> payload;
    const uint16_t count =
        size == 0
            ? 0
            : static_cast<uint16_t>(1 + data[0] % 8);
    capsid::protocol::append_u16(&payload, count);
    size_t offset = count == 0 ? 0 : 1;
    for (uint16_t index = 0; index < count; ++index) {
        const size_t remaining = size - offset;
        const size_t name_size =
            remaining == 0
                ? 0
                : static_cast<size_t>(data[offset] % 33);
        const size_t actual_name =
            name_size < remaining ? name_size : remaining;
        capsid::protocol::append_u16(
            &payload, static_cast<uint16_t>(actual_name));
        payload.insert(
            payload.end(),
            data + offset,
            data + offset + actual_name);
        offset += actual_name;

        const size_t value_remaining = size - offset;
        const size_t value_size =
            value_remaining == 0
                ? 0
                : static_cast<size_t>(data[offset] % 65);
        const size_t actual_value =
            value_size < value_remaining
                ? value_size
                : value_remaining;
        capsid::protocol::append_u32(
            &payload, static_cast<uint32_t>(actual_value));
        payload.insert(
            payload.end(),
            data + offset,
            data + offset + actual_value);
        offset += actual_value;
    }
    exercise(&payload[0], payload.size(), count / 2);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t wanted = 0;
    for (size_t index = 0; index < size && index < 4; ++index) {
        wanted =
            (wanted << 8) | static_cast<size_t>(data[index]);
    }
    exercise(data, size, wanted);
    if (size != 0) {
        const size_t skip =
            static_cast<size_t>(data[0]) % (size + 1);
        exercise(data + skip, size - skip, wanted);
    }
    exercise_structured(data, size);
    return 0;
}
