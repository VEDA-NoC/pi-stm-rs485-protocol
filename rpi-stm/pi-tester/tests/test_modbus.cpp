#include "modbus_rtu.hpp"
#include "stm_protocol.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& name)
{
    if (condition) {
        std::cout << "PASS: " << name << '\n';
    } else {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> with_crc(std::vector<std::uint8_t> frame)
{
    p2::append_modbus_crc(frame);
    return frame;
}

void test_crc()
{
    const std::vector<std::uint8_t> known{
        0x01, 0x03, 0x00, 0x00, 0x00, 0x0a,
    };
    expect(p2::modbus_crc16(known.data(), known.size()) == 0xcdc5,
           "Modbus published FC3 CRC vector");

    std::vector<std::uint8_t> frame = known;
    p2::append_modbus_crc(frame);
    expect(frame[6] == 0xc5 && frame[7] == 0xcd,
           "CRC is appended low byte first");
    expect(p2::has_valid_modbus_crc(frame), "valid CRC accepted");
    frame[2] ^= 0x01;
    expect(!p2::has_valid_modbus_crc(frame), "corrupted CRC rejected");
}

void test_read_request_and_response()
{
    const auto request = p2::build_read_input_request(
        3, STM_IR_BASE_STATE_START, STM_IR_BASE_STATE_COUNT);
    expect(request.size() == 8, "FC4 request length");
    expect(request[0] == 3 && request[1] == 4 && request[4] == 0 &&
               request[5] == STM_IR_BASE_STATE_COUNT,
           "FC4 request fields");
    expect(p2::has_valid_modbus_crc(request), "FC4 request CRC");

    const std::vector<std::uint16_t> expected_registers{
        4512,
        static_cast<std::uint16_t>(
            STM_STATE_TEMPERATURE_VALID | STM_STATE_OCCUPANCY_VALID |
            STM_STATE_OCCUPIED | STM_STATE_FIRE_ACTIVE |
            STM_STATE_EVENT_PENDING),
        37,
        stm_protocol_pack_actuator_status(STM_ACTUATOR_COMPLETE,
                                          STM_ACTUATOR_RUNNING),
        0x1234,
        0x5678,
    };
    std::vector<std::uint8_t> response{3, 4, 12};
    for (const auto value : expected_registers) {
        response.push_back(static_cast<std::uint8_t>(value >> 8));
        response.push_back(static_cast<std::uint8_t>(value & 0xffU));
    }
    response = with_crc(response);
    const auto parsed =
        p2::parse_read_input_response(response, 3, expected_registers.size());
    expect(parsed.ok, "FC4 response accepted");
    expect(parsed.registers == expected_registers,
           "FC4 register values decoded high byte first");
    expect(stm_protocol_u32_from_regs(parsed.registers[4],
                                      parsed.registers[5]) ==
               UINT32_C(0x12345678),
           "FC4 uint32 high register first");

    response[3] ^= 0x01;
    const auto bad_crc =
        p2::parse_read_input_response(response, 3, expected_registers.size());
    expect(!bad_crc.ok && bad_crc.error.find("CRC") != std::string::npos,
           "FC4 bad CRC rejected");
}

void test_write_request_and_response()
{
    std::vector<std::uint16_t> registers(STM_HR_COMMAND_COUNT, 0);
    registers[STM_HR_COMMAND_OPCODE] = STM_COMMAND_START_STAGE2;
    registers[STM_HR_COMMAND_ID_HI] = 0;
    registers[STM_HR_COMMAND_ID_LO] = 1001;
    registers[STM_HR_COMMAND_ORIGIN_AND_FLAGS] =
        stm_protocol_pack_origin_and_flags(STM_COMMAND_ORIGIN_QT_ADMIN, 0);

    const auto request = p2::build_write_multiple_request(
        3, STM_HR_COMMAND_START, registers);
    expect(request.size() == 25, "FC16 eight-register request length");
    expect(request[0] == 3 && request[1] == 16 && request[2] == 0x01 &&
               request[3] == 0x00 && request[4] == 0 &&
               request[5] == STM_HR_COMMAND_COUNT && request[6] == 16,
           "FC16 request header");
    expect(request[7] == 0 && request[8] == STM_COMMAND_START_STAGE2 &&
               request[11] == 0x03 && request[12] == 0xe9,
           "FC16 command register encoding");
    expect(p2::has_valid_modbus_crc(request), "FC16 request CRC");

    const auto response =
        with_crc({3, 16, 0x01, 0x00, 0x00, STM_HR_COMMAND_COUNT});
    const auto parsed = p2::parse_write_multiple_response(
        response, 3, STM_HR_COMMAND_START, STM_HR_COMMAND_COUNT);
    expect(parsed.ok, "FC16 echo response accepted");

    const auto wrong_address = p2::parse_write_multiple_response(
        response, 3, 0x0101, STM_HR_COMMAND_COUNT);
    expect(!wrong_address.ok, "FC16 wrong echo address rejected");
}

void test_time_sync_command()
{
    const std::uint32_t epoch_seconds = UINT32_C(1700000000);
    std::vector<std::uint16_t> registers(STM_HR_COMMAND_COUNT, 0);
    registers[STM_HR_COMMAND_OPCODE] = STM_COMMAND_SYNC_TIME;
    registers[STM_HR_COMMAND_ARGUMENT0] = stm_protocol_u32_hi(epoch_seconds);
    registers[STM_HR_COMMAND_ARGUMENT1] = stm_protocol_u32_lo(epoch_seconds);

    const auto request = p2::build_write_multiple_request(
        3, STM_HR_COMMAND_START, registers);
    expect(request[7] == 0 && request[8] == STM_COMMAND_SYNC_TIME,
           "SYNC_TIME opcode encoding");
    expect(stm_protocol_u32_from_regs(registers[STM_HR_COMMAND_ARGUMENT0],
                                      registers[STM_HR_COMMAND_ARGUMENT1]) ==
               epoch_seconds,
           "SYNC_TIME Unix epoch seconds high register first");
    expect(p2::has_valid_modbus_crc(request), "SYNC_TIME request CRC");
}

void test_exception_and_contract()
{
    const auto exception = with_crc({3, 0x84, 0x02});
    const auto parsed =
        p2::parse_read_input_response(exception, 3, STM_IR_BASE_STATE_COUNT);
    expect(!parsed.ok && parsed.is_exception &&
               parsed.exception_code == 2 &&
               parsed.error.find("Illegal Data Address") != std::string::npos,
           "Modbus exception decoded");

    expect(stm_protocol_ir_array_index(STM_IR_IDENTITY_START) == 0x0040,
           "FC4 PDU address to STM input index");
    expect(stm_protocol_hr_array_index(STM_HR_COMMAND_START) == 0,
           "FC16 PDU address to STM holding index");
    expect(STM_IR_REGION_COUNT == 160 && STM_HR_REGION_COUNT == 96,
           "separate STM register region sizes");
    expect(STM_IR_ID_UID_WORD0_HI == 4 &&
               STM_IR_ID_UID_WORD0_LO == 5 &&
               STM_IR_ID_UID_WORD2_LO == 9,
           "UID word0-word2 order fixed");
    expect(STM_STATE_EVENT_PENDING == UINT16_C(0x0020) &&
               STM_STATE_EVENT_OVERFLOW == UINT16_C(0x0040) &&
               STM_STATE_REARM_REQUIRED == UINT16_C(0x0400) &&
               STM_STATE_RESERVED_MASK == UINT16_C(0xf800),
           "compact state flag bit layout");
    expect(STM_COMMAND_STATUS_REJECTED == 5 &&
               STM_COMMAND_REASON_ID_CONFLICT == 8,
           "command lifecycle and ID conflict contract");
    expect(STM_PROTOCOL_EVENT_PAYLOAD_MAX_BYTES == 8 &&
               STM_EVENT_PAYLOAD_WORDS_FIRE <=
                   STM_PROTOCOL_EVENT_PAYLOAD_MAX_REGISTERS &&
               STM_EVENT_PAYLOAD_WORDS_QUEUE_OVERFLOW <=
                   STM_PROTOCOL_EVENT_PAYLOAD_MAX_REGISTERS,
           "event payload schemas fit reserved window");
}

}  // namespace

int main()
{
    test_crc();
    test_read_request_and_response();
    test_write_request_and_response();
    test_time_sync_command();
    test_exception_and_contract();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All protocol unit tests passed\n";
    return 0;
}
