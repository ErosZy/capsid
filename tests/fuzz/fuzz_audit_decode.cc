#include "protocol.h"
#include "capsid/runtime.h"

#include <stdint.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

void append_string(std::vector<uint8_t> *payload,
                   const uint8_t *data,
                   size_t size) {
    capsid::protocol::append_u16(
        payload, static_cast<uint16_t>(size));
    if (size != 0) {
        payload->insert(payload->end(), data, data + size);
    }
}

void verify_field(const capsid_bytes &field,
                  const uint8_t *data,
                  size_t size) {
    if (field.size == 0) {
        if (field.data != NULL) {
            std::abort();
        }
        return;
    }
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(data);
    const uintptr_t end = begin + size;
    const uintptr_t field_begin =
        reinterpret_cast<uintptr_t>(field.data);
    if (end < begin || field_begin < begin ||
        field_begin > end || field.size > end - field_begin) {
        std::abort();
    }
}

void exercise(const uint8_t *data, size_t size) {
    capsid_event event = {};
    event.struct_size = sizeof(event);
    event.type = CAPSID_EVENT_AUDIT;
    event.request_id = 17;
    event.payload.data = data;
    event.payload.size = size;

    capsid_audit_record record;
    capsid_audit_record_init(&record);
    record.worker_id = UINT64_C(0xfedcba9876543210);
    const capsid_result result =
        capsid_audit_record_decode(&event, &record);
    if (result != CAPSID_OK) {
        if (record.worker_id !=
            UINT64_C(0xfedcba9876543210)) {
            std::abort();
        }
        return;
    }
    if (record.struct_size != sizeof(record) ||
        record.version != 1 ||
        record.request_id != event.request_id ||
        record.worker_id == 0 ||
        record.stage < CAPSID_AUDIT_STAGE_BUILD ||
        record.stage > CAPSID_AUDIT_STAGE_QUERY ||
        record.decision < CAPSID_AUDIT_DENY ||
        record.decision > CAPSID_AUDIT_PARTIAL ||
        record.manifest_hash.size != 64) {
        std::abort();
    }
    const capsid_bytes fields[] = {
        record.application_identity,
        record.module,
        record.capability,
        record.resource_kind,
        record.resource,
        record.manifest_hash
    };
    for (size_t index = 0;
         index < sizeof(fields) / sizeof(fields[0]);
         ++index) {
        verify_field(fields[index], data, size);
    }
}

void exercise_structured(const uint8_t *data, size_t size) {
    std::vector<uint8_t> payload;
    capsid::protocol::append_u32(&payload, 1);
    capsid::protocol::append_u32(
        &payload,
        CAPSID_AUDIT_STAGE_BUILD +
            (size == 0 ? 0 : data[0] % 4));
    capsid::protocol::append_u32(
        &payload,
        size < 2 ? CAPSID_AUDIT_DENY : data[1] % 4);
    capsid::protocol::append_u64(&payload, 1);
    capsid::protocol::append_u32(
        &payload, size < 3 ? 0 : data[2]);
    capsid::protocol::append_u32(
        &payload, CAPSID_CAPABILITY_POLICY_VERSION);
    const size_t field_size = size < 32 ? size : 32;
    append_string(&payload, data, field_size);
    append_string(&payload, NULL, 0);
    append_string(&payload, NULL, 0);
    append_string(&payload, NULL, 0);
    append_string(&payload, NULL, 0);
    const std::string manifest(
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef");
    append_string(
        &payload,
        reinterpret_cast<const uint8_t *>(manifest.data()),
        manifest.size());
    exercise(&payload[0], payload.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const uint8_t *data,
    size_t size) {
    exercise(data, size);
    exercise_structured(data, size);
    return 0;
}
