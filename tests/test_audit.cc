#include "protocol.h"
#include "capsid/runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "test-audit: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void append_string(
    std::vector<uint8_t> *payload,
    const std::string &value) {
    capsid::protocol::append_u16(
        payload, static_cast<uint16_t>(value.size()));
    payload->insert(
        payload->end(), value.begin(), value.end());
}

std::vector<uint8_t> valid_payload() {
    std::vector<uint8_t> payload;
    capsid::protocol::append_u32(&payload, 1);
    capsid::protocol::append_u32(
        &payload, CAPSID_AUDIT_STAGE_OPERATION);
    capsid::protocol::append_u32(
        &payload, CAPSID_AUDIT_DENY);
    capsid::protocol::append_u64(&payload, 1234);
    capsid::protocol::append_u32(&payload, 77);
    capsid::protocol::append_u32(
        &payload, CAPSID_CAPABILITY_POLICY_VERSION);
    append_string(&payload, "application-a");
    append_string(&payload, "");
    append_string(&payload, "net");
    append_string(&payload, "host");
    append_string(&payload, "api.example.com:443");
    append_string(
        &payload,
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef");
    return payload;
}

capsid_result decode(
    const std::vector<uint8_t> &payload,
    capsid_audit_record *record) {
    capsid_event event = {};
    event.struct_size = sizeof(event);
    event.type = CAPSID_EVENT_AUDIT;
    event.request_id = 42;
    event.payload.data =
        payload.empty() ? NULL : &payload[0];
    event.payload.size = payload.size();
    return capsid_audit_record_decode(&event, record);
}

}  // namespace

int main() {
    const std::vector<uint8_t> valid = valid_payload();
    capsid_audit_record record;
    capsid_audit_record_init(&record);
    require(
        decode(valid, &record) == CAPSID_OK,
        "valid audit payload was rejected");
    require(
        record.version == 1 &&
            record.worker_id == 1234 &&
            record.request_id == 42 &&
            record.rule_id == 77 &&
            record.capability.size == 3 &&
            std::memcmp(
                record.capability.data, "net", 3) == 0 &&
            record.manifest_hash.size == 64,
        "valid audit fields decoded incorrectly");

    for (size_t size = 0; size < valid.size(); ++size) {
        std::vector<uint8_t> truncated(
            valid.begin(), valid.begin() + size);
        capsid_audit_record_init(&record);
        require(
            decode(truncated, &record) != CAPSID_OK,
            "truncated audit payload was accepted");
    }

    std::vector<uint8_t> trailing = valid;
    trailing.push_back(0);
    capsid_audit_record_init(&record);
    require(
        decode(trailing, &record) == CAPSID_PROTOCOL_ERROR,
        "audit trailing byte was accepted");

    std::vector<uint8_t> invalid_stage = valid;
    invalid_stage[4] = 0xff;
    capsid_audit_record_init(&record);
    require(
        decode(invalid_stage, &record) ==
            CAPSID_PROTOCOL_ERROR,
        "unknown audit stage was accepted");

    capsid_audit_record too_small;
    capsid_audit_record_init(&too_small);
    too_small.struct_size = sizeof(too_small) - 1;
    require(
        decode(valid, &too_small) ==
            CAPSID_INVALID_ARGUMENT,
        "short audit output descriptor was accepted");
    return 0;
}
