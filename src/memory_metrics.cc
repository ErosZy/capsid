#include "protocol.h"
#include "capsid/runtime.h"

#include <cstring>

extern "C" {

void capsid_memory_metrics_init(capsid_memory_metrics *metrics) {
    if (!metrics) {
        return;
    }
    std::memset(metrics, 0, sizeof(*metrics));
    metrics->struct_size = sizeof(*metrics);
}

capsid_result capsid_memory_metrics_decode(
    const capsid_event *event,
    capsid_memory_metrics *out_metrics) {
    if (!event || event->type != CAPSID_EVENT_MEMORY_METRICS ||
        !out_metrics ||
        out_metrics->struct_size < sizeof(capsid_memory_metrics) ||
        !event->payload.data) {
        return CAPSID_INVALID_ARGUMENT;
    }

    capsid_memory_metrics decoded;
    capsid_memory_metrics_init(&decoded);
    const uint8_t *cursor = event->payload.data;
    const uint8_t *end = cursor + event->payload.size;
    if (!capsid::protocol::read_u32(
            &cursor, end, &decoded.version) ||
        decoded.version != CAPSID_MEMORY_METRICS_VERSION) {
        return CAPSID_PROTOCOL_ERROR;
    }
    uint64_t *fields[] = {
        &decoded.malloc_size,
        &decoded.malloc_limit,
        &decoded.memory_used_size,
        &decoded.atom_count,
        &decoded.atom_size,
        &decoded.string_count,
        &decoded.string_size,
        &decoded.object_count,
        &decoded.object_size,
        &decoded.property_count,
        &decoded.property_size,
        &decoded.shape_count,
        &decoded.shape_size,
        &decoded.js_function_count,
        &decoded.js_function_size,
        &decoded.js_function_code_size,
        &decoded.binary_object_count,
        &decoded.binary_object_size
    };
    for (size_t index = 0;
         index < sizeof(fields) / sizeof(fields[0]);
         ++index) {
        if (!capsid::protocol::read_u64(
                &cursor, end, fields[index])) {
            return CAPSID_PROTOCOL_ERROR;
        }
    }
    if (cursor != end) {
        return CAPSID_PROTOCOL_ERROR;
    }
    *out_metrics = decoded;
    return CAPSID_OK;
}

}  // extern "C"
