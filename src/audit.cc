#include "protocol.h"
#include "capsid/runtime.h"

#include <cstring>

extern "C" {

void capsid_audit_record_init(capsid_audit_record *record) {
    if (!record) {
        return;
    }
    std::memset(record, 0, sizeof(*record));
    record->struct_size = sizeof(*record);
}

capsid_result capsid_audit_record_decode(
    const capsid_event *event,
    capsid_audit_record *out_record) {
    if (!event || event->type != CAPSID_EVENT_AUDIT ||
        !out_record ||
        out_record->struct_size < sizeof(capsid_audit_record) ||
        !event->payload.data) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (event->payload.size >
        capsid::protocol::kMaxPayloadSize) {
        return CAPSID_PROTOCOL_ERROR;
    }
    capsid_audit_record decoded;
    capsid_audit_record_init(&decoded);
    const uint8_t *cursor = event->payload.data;
    const uint8_t *end = cursor + event->payload.size;
    uint32_t stage = 0;
    uint32_t decision = 0;
    if (!capsid::protocol::read_u32(
            &cursor, end, &decoded.version) ||
        decoded.version != 1 ||
        !capsid::protocol::read_u32(&cursor, end, &stage) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decision) ||
        !capsid::protocol::read_u64(
            &cursor, end, &decoded.worker_id) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.rule_id) ||
        !capsid::protocol::read_u32(
            &cursor, end, &decoded.policy_version)) {
        return CAPSID_PROTOCOL_ERROR;
    }
    decoded.stage = static_cast<capsid_audit_stage>(stage);
    decoded.decision =
        static_cast<capsid_audit_decision>(decision);
    decoded.request_id = event->request_id;
    if (decoded.worker_id == 0 ||
        stage < CAPSID_AUDIT_STAGE_BUILD ||
        stage > CAPSID_AUDIT_STAGE_QUERY ||
        decision > CAPSID_AUDIT_PARTIAL) {
        return CAPSID_PROTOCOL_ERROR;
    }

    capsid_bytes *fields[] = {
        &decoded.application_identity,
        &decoded.module,
        &decoded.capability,
        &decoded.resource_kind,
        &decoded.resource,
        &decoded.manifest_hash
    };
    for (size_t index = 0;
         index < sizeof(fields) / sizeof(fields[0]);
         ++index) {
        uint16_t size = 0;
        if (!capsid::protocol::read_u16(
                &cursor, end, &size) ||
            static_cast<size_t>(end - cursor) < size) {
            return CAPSID_PROTOCOL_ERROR;
        }
        fields[index]->data = size == 0 ? NULL : cursor;
        fields[index]->size = size;
        cursor += size;
    }
    if (cursor != end || decoded.manifest_hash.size != 64) {
        return CAPSID_PROTOCOL_ERROR;
    }
    *out_record = decoded;
    return CAPSID_OK;
}

}  // extern "C"
