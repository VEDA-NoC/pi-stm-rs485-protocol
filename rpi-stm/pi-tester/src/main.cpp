#include "modbus_rtu.hpp"
#include "stm_protocol.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string device = "/dev/ttyUSB0";
    std::uint8_t slave = 1;
    int timeout_ms = static_cast<int>(STM_PROTOCOL_RESPONSE_TIMEOUT_MS);
    bool dry_run = false;
    bool kernel_rs485 = false;
    bool self_test = false;
};

struct Field {
    std::uint16_t offset;
    std::uint16_t words;
    const char* name;
    const char* type;
    const char* meaning;
};

struct ReadWindow {
    char key;
    const char* title;
    std::uint16_t start;
    std::uint16_t count;
    const Field* fields;
    std::size_t field_count;
};

constexpr Field kBaseFields[] = {
    {0, 1, "temperature_centi_c", "uint16", "현재 온도 ×100; 4512=45.12°C"},
    {1, 1, "state_flags", "uint16 bitmap", "유효성·점유·화재·event·fault"},
    {2, 1, "temperature_age_ms", "uint16", "측정 후 경과 ms; 0xffff=unknown"},
    {3, 1, "actuator_status", "uint16", "low byte=stage1, high byte=stage2"},
    {4, 2, "oldest_pending_event_sequence", "uint32", "다음에 읽고 ACK할 event; 0=없음"},
};

constexpr Field kResultFields[] = {
    {0, 2, "result_command_id", "uint32", "결과가 가리키는 command ID"},
    {2, 1, "command_status", "uint16 enum", "accepted/running/completed 등"},
    {3, 1, "command_reason", "uint16 enum", "실패·거부 이유"},
    {4, 1, "result_actuator_status", "uint16", "결과 시점 stage1/stage2 상태"},
    {5, 1, "result_reserved", "uint16", "v0.1은 0"},
};

constexpr Field kEventFields[] = {
    {0, 1, "pending_event_count", "uint16", "ACK 대기 event 수"},
    {1, 1, "event_status_flags", "uint16 bitmap", "queue overflow 상태"},
    {2, 2, "dropped_event_count", "uint32", "유실된 누적 event 수"},
    {4, 2, "oldest_available_event_sequence", "uint32", "실제로 남아 있는 첫 event"},
    {6, 2, "event_sequence", "uint32", "현재 slot event 번호"},
    {8, 2, "event_boot_session_id", "uint32", "event의 STM boot session"},
    {10, 4, "event_local_tick_ms", "uint64", "event 발생 monotonic ms"},
    {14, 1, "event_type", "uint16 enum", "fire/occupancy/stage/fault/restart"},
    {15, 1, "event_state_and_severity", "uint16", "low=state, high=severity"},
    {16, 1, "event_payload_word_count", "uint16", "payload 유효 word 수 0..4"},
    {17, 4, "event_payload", "uint16[4]", "event type별 값"},
};

constexpr Field kIdentityFields[] = {
    {0, 1, "protocol_version", "uint16", "high byte=major, low byte=minor"},
    {1, 1, "firmware_version", "uint16", "high byte=major, low byte=minor"},
    {2, 2, "capability_bitmap", "uint32", "설치된 hardware 기능"},
    {4, 2, "device_uid_word0", "uint32", "STM UID word0, high word 먼저"},
    {6, 2, "device_uid_word1", "uint32", "STM UID word1, high word 먼저"},
    {8, 2, "device_uid_word2", "uint32", "STM UID word2, high word 먼저"},
    {10, 2, "boot_session_id", "uint32", "Pi가 reboot마다 할당"},
    {12, 1, "reset_reason", "uint16 bitmap", "power/watchdog/software 등"},
    {13, 1, "hardware_revision", "uint16", "high byte=major, low byte=minor"},
    {14, 2, "identity_reserved", "uint16[2]", "v0.1은 0"},
};

constexpr Field kSensorFields[] = {
    {0, 1, "distance_mm", "uint16", "최신 초음파 거리, 1mm"},
    {1, 1, "distance_age_ms", "uint16", "측정 후 경과 ms; 0xffff=unknown"},
    {2, 1, "sensor_status_flags", "uint16 bitmap", "valid/stale/fault/noisy"},
    {3, 4, "occupancy_changed_tick_ms", "uint64", "점유 상태가 마지막으로 바뀐 boot tick"},
    {7, 1, "sensor_detail_reserved", "uint16", "v0.1은 0"},
};

constexpr ReadWindow kWindows[] = {
    {'1', "Base state", STM_IR_BASE_STATE_START, STM_IR_BASE_STATE_COUNT,
     kBaseFields, std::size(kBaseFields)},
    {'2', "Command result", STM_IR_COMMAND_RESULT_START,
     STM_IR_COMMAND_RESULT_COUNT, kResultFields, std::size(kResultFields)},
    {'3', "Event status", STM_IR_EVENT_STATUS_START,
     STM_IR_EVENT_STATUS_USED_COUNT, kEventFields, std::size(kEventFields)},
    {'4', "Identity", STM_IR_IDENTITY_START, STM_IR_IDENTITY_COUNT,
     kIdentityFields, std::size(kIdentityFields)},
    {'5', "Sensor detail", STM_IR_SENSOR_DETAIL_START,
     STM_IR_SENSOR_DETAIL_COUNT, kSensorFields, std::size(kSensorFields)},
};

std::string trim(std::string value)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string prompt(const std::string& text)
{
    std::cout << text;
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw std::runtime_error("입력이 종료됐습니다.");
    }
    return trim(line);
}

std::uint64_t parse_integer(const std::string& text, std::uint64_t minimum,
                            std::uint64_t maximum)
{
    std::size_t used = 0;
    const unsigned long long value = std::stoull(text, &used, 0);
    if (used != text.size() || value < minimum || value > maximum) {
        throw std::out_of_range("입력 범위를 벗어났습니다.");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint64_t prompt_integer(const std::string& text, std::uint64_t minimum,
                             std::uint64_t maximum,
                             std::optional<std::uint64_t> default_value =
                                 std::nullopt)
{
    while (true) {
        try {
            const std::string line = prompt(text);
            if (line.empty() && default_value.has_value()) {
                return *default_value;
            }
            return parse_integer(line, minimum, maximum);
        } catch (const std::exception&) {
            std::cout << "  입력 오류: " << minimum << ".." << maximum
                      << " 또는 0x 접두사 16진수를 입력하십시오.\n";
        }
    }
}

std::uint32_t current_unix_epoch_seconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now).count();
    if (seconds < 0 ||
        seconds > static_cast<std::int64_t>(
                      std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("현재 system clock이 Unix epoch uint32 범위를 벗어났습니다.");
    }
    return static_cast<std::uint32_t>(seconds);
}

bool prompt_yes_no(const std::string& text, bool default_yes)
{
    while (true) {
        const std::string line = prompt(text);
        if (line.empty()) {
            return default_yes;
        }
        if (line == "y" || line == "Y" || line == "yes") {
            return true;
        }
        if (line == "n" || line == "N" || line == "no") {
            return false;
        }
        std::cout << "  y 또는 n을 입력하십시오.\n";
    }
}

std::string hex16(std::uint16_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

std::string hex32(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::uint32_t get_u32(const std::vector<std::uint16_t>& regs,
                      std::size_t offset)
{
    return stm_protocol_u32_from_regs(regs.at(offset), regs.at(offset + 1));
}

std::uint64_t get_u64(const std::vector<std::uint16_t>& regs,
                      std::size_t offset)
{
    return stm_protocol_u64_from_regs(&regs.at(offset));
}

std::string actuator_name(std::uint8_t state)
{
    switch (state) {
        case STM_ACTUATOR_NOT_PRESENT:
            return "NOT_PRESENT";
        case STM_ACTUATOR_IDLE:
            return "IDLE";
        case STM_ACTUATOR_RUNNING:
            return "RUNNING";
        case STM_ACTUATOR_COMPLETE:
            return "COMPLETE";
        case STM_ACTUATOR_FAILED:
            return "FAILED";
        case STM_ACTUATOR_STOPPED:
            return "STOPPED";
        case STM_ACTUATOR_INTERLOCKED:
            return "INTERLOCKED";
        case STM_ACTUATOR_REARM_REQUIRED:
            return "REARM_REQUIRED";
        default:
            return "UNKNOWN(" + std::to_string(state) + ")";
    }
}

std::string command_status_name(std::uint16_t status)
{
    switch (status) {
        case STM_COMMAND_STATUS_NONE:
            return "NONE";
        case STM_COMMAND_STATUS_ACCEPTED:
            return "ACCEPTED";
        case STM_COMMAND_STATUS_RUNNING:
            return "RUNNING";
        case STM_COMMAND_STATUS_COMPLETED:
            return "COMPLETED";
        case STM_COMMAND_STATUS_FAILED:
            return "FAILED";
        case STM_COMMAND_STATUS_REJECTED:
            return "REJECTED";
        default:
            return "UNKNOWN(" + std::to_string(status) + ")";
    }
}

std::string command_reason_name(std::uint16_t reason)
{
    switch (reason) {
        case STM_COMMAND_REASON_NONE:
            return "NONE";
        case STM_COMMAND_REASON_UNSUPPORTED:
            return "UNSUPPORTED";
        case STM_COMMAND_REASON_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case STM_COMMAND_REASON_BUSY:
            return "BUSY";
        case STM_COMMAND_REASON_INTERLOCK:
            return "INTERLOCK";
        case STM_COMMAND_REASON_FIRE_STILL_ACTIVE:
            return "FIRE_STILL_ACTIVE";
        case STM_COMMAND_REASON_REARM_REQUIRED:
            return "REARM_REQUIRED";
        case STM_COMMAND_REASON_LOCAL_FAULT:
            return "LOCAL_FAULT";
        case STM_COMMAND_REASON_ID_CONFLICT:
            return "ID_CONFLICT";
        default:
            return "UNKNOWN(" + std::to_string(reason) + ")";
    }
}

void print_window_table(const ReadWindow& window)
{
    std::cout << "\n[" << window.title << "] FC4\n"
              << "PDU logical start : " << hex16(window.start) << " ("
              << window.start << ")\n"
              << "3xxxx reference   : " << 30001U + window.start
              << " (사람/도구용 표기, wire에 포함되지 않음)\n"
              << "STM input index   : "
              << stm_protocol_ir_array_index(window.start) << "\n"
              << "register count    : " << window.count << "\n\n"
              << "offset  PDU addr  3xxxx  STM idx  words  type          variable"
                 "                         meaning\n"
              << "------  --------  -----  -------  -----  ------------  "
                 "-------------------------------  -------------------------\n";
    for (std::size_t i = 0; i < window.field_count; ++i) {
        const Field& field = window.fields[i];
        const std::uint16_t address =
            static_cast<std::uint16_t>(window.start + field.offset);
        std::cout << std::setw(6) << field.offset << "  " << std::setw(8)
                  << hex16(address) << "  " << std::setw(5)
                  << 30001U + address << "  " << std::setw(7)
                  << stm_protocol_ir_array_index(address) << "  "
                  << std::setw(5) << field.words << "  " << std::left
                  << std::setw(12) << field.type << "  " << std::setw(31)
                  << field.name << "  " << field.meaning << std::right
                  << '\n';
    }
    switch (window.key) {
        case '1':
            std::cout
                << "\nstate_flags bits: b0 temp_valid, b1 temp_stale, "
                   "b2 occupancy_valid,\n"
                << "  b3 occupied, b4 fire_active, b5 event_pending, "
                   "b6 event_overflow,\n"
                << "  b7 sensor_fault, b8 actuator_fault, b9 restarted, "
                   "b10 rearm_required\n"
                << "bit 값은 OR하여 입력합니다. 예: b0|b3|b4 = 0x0019\n"
                << "actuator byte 값: 0 absent, 1 idle, 2 running, 3 complete, "
                   "4 failed,\n"
                << "  5 stopped, 6 interlocked, 7 rearm_required\n";
            break;
        case '2':
            std::cout
                << "\ncommand_status: 0 none, 1 accepted, 2 running, "
                   "3 completed,\n"
                << "  4 failed, 5 rejected\n"
                << "command_reason: 0 none, 1 unsupported, 2 invalid_argument, "
                   "3 busy,\n"
                << "  4 interlock, 5 fire_still_active, 6 rearm_required, "
                   "7 local_fault,\n"
                << "  8 id_conflict (같은 command ID에 다른 payload)\n";
            break;
        case '3':
            std::cout
                << "\nevent_type: 1 fire_started, 2 fire_cleared, "
                   "3 occupancy_changed,\n"
                << "  4 stage1_changed, 5 stage2_changed, 6 sensor_fault, "
                   "7 actuator_fault,\n"
                << "  8 restarted, 9 queue_overflow\n"
                << "state low byte: 0 occurred, 1 started, 2 ended; "
                   "severity high byte: 0 info, 1 warning, 2 critical\n";
            break;
        case '4':
            std::cout
                << "\ncapability bits: b0 IR temp, b1 ultrasonic, b2 LED, "
                   "b3 buzzer,\n"
                << "  b4 display, b5 stage1 servo, b6 stage2 pump\n"
                << "UID 순서: word0 high/low | word1 high/low | "
                   "word2 high/low\n";
            break;
        case '5':
            std::cout
                << "\nsensor_status bits: b0 distance_valid, b1 stale, "
                   "b2 out_of_range,\n"
                << "  b3 ultrasonic_fault, b4 infrared_fault, b5 noisy, "
                   "b6 occupancy_tick_valid\n";
            break;
        default:
            break;
    }
}

void print_register_values(std::uint16_t start,
                           const std::vector<std::uint16_t>& regs,
                           bool input_registers)
{
    std::cout << "\nregister 원시값 (구분자 | 는 16-bit 경계)\n"
              << "HEX: " << p2::registers_hex(regs) << "\nDEC: ";
    for (std::size_t i = 0; i < regs.size(); ++i) {
        if (i != 0) {
            std::cout << " | ";
        }
        std::cout << regs[i];
    }
    std::cout << "\n\n주소별 값\n";
    for (std::size_t i = 0; i < regs.size(); ++i) {
        const std::uint16_t address =
            static_cast<std::uint16_t>(start + i);
        const std::uint32_t reference =
            (input_registers ? 30001U : 40001U) + address;
        const std::uint16_t array_index =
            input_registers ? stm_protocol_ir_array_index(address)
                            : stm_protocol_hr_array_index(address);
        std::cout << "  PDU " << hex16(address) << " / ref " << reference
                  << " / STM index " << array_index << " = " << hex16(regs[i])
                  << " = " << regs[i] << '\n';
    }
}

void print_frame(const std::vector<std::uint8_t>& frame, bool is_write,
                 std::uint16_t start, std::uint16_t count)
{
    const auto byte_hex = [](std::uint8_t value) {
        std::ostringstream out;
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(value);
        return out.str();
    };
    std::cout << "\n실제 RTU 송신 frame\n"
              << "field: slave | FC | start address | register count";
    if (is_write) {
        std::cout << " | byte count | register data";
    }
    std::cout << " | CRC low high\n"
              << "grouped: [" << byte_hex(frame[0]) << "] | ["
              << byte_hex(frame[1]) << "] | [" << byte_hex(frame[2]) << ' '
              << byte_hex(frame[3]) << "] | [" << byte_hex(frame[4]) << ' '
              << byte_hex(frame[5]) << ']';
    if (is_write) {
        std::cout << " | [" << byte_hex(frame[6]) << "] | ";
        for (std::size_t i = 7; i + 2 < frame.size(); i += 2) {
            if (i != 7) {
                std::cout << " | ";
            }
            std::cout << '[' << byte_hex(frame[i]) << ' '
                      << byte_hex(frame[i + 1]) << ']';
        }
    }
    std::cout << " | [" << byte_hex(frame[frame.size() - 2]) << ' '
              << byte_hex(frame[frame.size() - 1]) << "]\n"
              << "HEX bytes: " << p2::bytes_hex(frame) << "\n"
              << "DEC bytes: " << p2::bytes_decimal(frame) << "\n"
              << "해석: slave=" << static_cast<unsigned>(frame[0])
              << ", FC=" << static_cast<unsigned>(frame[1])
              << ", PDU start=" << hex16(start) << " (" << start
              << "), count=" << count << ", CRC wire order=low→high\n";
}

std::map<std::size_t, std::uint16_t> prompt_expectations(
    std::uint16_t register_count)
{
    std::cout
        << "\n선택 사항: 원시 register 기대값을 비교할 수 있습니다.\n"
        << "형식은 offset=value이며 쉼표로 구분합니다.\n"
        << "예: 0=4512,1=0x003d  (Enter만 누르면 비교하지 않음)\n";
    while (true) {
        const std::string line = prompt("기대값> ");
        std::map<std::size_t, std::uint16_t> result;
        if (line.empty()) {
            return result;
        }
        try {
            std::istringstream input(line);
            std::string item;
            while (std::getline(input, item, ',')) {
                const std::size_t separator = item.find('=');
                if (separator == std::string::npos) {
                    throw std::invalid_argument("=");
                }
                const std::size_t offset = static_cast<std::size_t>(
                    parse_integer(trim(item.substr(0, separator)), 0,
                                  register_count - 1U));
                const std::uint16_t value = static_cast<std::uint16_t>(
                    parse_integer(trim(item.substr(separator + 1)), 0, 0xffff));
                result[offset] = value;
            }
            return result;
        } catch (const std::exception&) {
            std::cout << "  형식 오류입니다. 예: 0=4512,1=0x003d\n";
        }
    }
}

void compare_expectations(
    const std::vector<std::uint16_t>& regs,
    const std::map<std::size_t, std::uint16_t>& expected)
{
    if (expected.empty()) {
        std::cout << "\n기대값 비교: SKIP (통신·CRC·schema만 검사)\n";
        return;
    }
    bool all_match = true;
    std::cout << "\n기대값 비교\n";
    for (const auto& [offset, value] : expected) {
        const bool match = regs.at(offset) == value;
        all_match = all_match && match;
        std::cout << "  offset " << offset << ": expected " << hex16(value)
                  << " (" << value << "), actual " << hex16(regs.at(offset))
                  << " (" << regs.at(offset) << ") -> "
                  << (match ? "PASS" : "FAIL") << '\n';
    }
    std::cout << "비교 결과: " << (all_match ? "PASS" : "FAIL") << '\n';
}

void decode_base(const std::vector<std::uint16_t>& regs)
{
    const double temperature = regs.at(0) / 100.0;
    const std::uint16_t flags = regs.at(1);
    const std::uint16_t actuator = regs.at(3);
    std::cout << std::fixed << std::setprecision(2)
              << "temperature_centi_c: " << regs[0] << " -> " << temperature
              << "°C\n"
              << "temperature_age_ms: ";
    if (regs[2] == STM_PROTOCOL_TEMPERATURE_AGE_UNKNOWN) {
        std::cout << "unknown/stale\n";
    } else {
        std::cout << regs[2] << "ms\n";
    }
    std::cout << "state_flags: " << hex16(flags)
              << "\n  temperature_valid="
              << ((flags & STM_STATE_TEMPERATURE_VALID) != 0U)
              << ", temperature_stale="
              << ((flags & STM_STATE_TEMPERATURE_STALE) != 0U)
              << ", occupancy_valid="
              << ((flags & STM_STATE_OCCUPANCY_VALID) != 0U)
              << ", occupied=" << ((flags & STM_STATE_OCCUPIED) != 0U)
              << ", fire_active=" << ((flags & STM_STATE_FIRE_ACTIVE) != 0U)
              << ", event_pending="
              << ((flags & STM_STATE_EVENT_PENDING) != 0U)
              << ", event_overflow="
              << ((flags & STM_STATE_EVENT_OVERFLOW) != 0U)
              << ", sensor_fault="
              << ((flags & STM_STATE_SENSOR_FAULT) != 0U)
              << ", actuator_fault="
              << ((flags & STM_STATE_ACTUATOR_FAULT) != 0U)
              << ", restarted="
              << ((flags & STM_STATE_DEVICE_RESTARTED) != 0U)
              << ", rearm_required="
              << ((flags & STM_STATE_REARM_REQUIRED) != 0U) << '\n'
              << "actuator_status: stage1="
              << actuator_name(stm_protocol_stage1_from_status(actuator))
              << ", stage2="
              << actuator_name(stm_protocol_stage2_from_status(actuator))
              << '\n'
              << "oldest_pending_event_sequence: "
              << get_u32(regs, 4) << " (" << hex32(get_u32(regs, 4))
              << ")\n";
}

void decode_result(const std::vector<std::uint16_t>& regs)
{
    const std::uint32_t id = get_u32(regs, 0);
    std::cout << "result_command_id: " << id << " (" << hex32(id) << ")\n"
              << "command_status: " << command_status_name(regs.at(2))
              << " (" << regs[2] << ")\n"
              << "command_reason: " << command_reason_name(regs.at(3))
              << " (" << regs[3] << ")\n"
              << "actuator: stage1="
              << actuator_name(stm_protocol_stage1_from_status(regs.at(4)))
              << ", stage2="
              << actuator_name(stm_protocol_stage2_from_status(regs.at(4)))
              << '\n';
}

void decode_event(const std::vector<std::uint16_t>& regs)
{
    std::cout << "pending_event_count: " << regs.at(0) << '\n'
              << "event_status_flags: " << hex16(regs.at(1)) << '\n'
              << "dropped_event_count: " << get_u32(regs, 2) << '\n'
              << "oldest_available_event_sequence: " << get_u32(regs, 4)
              << '\n'
              << "event_sequence: " << get_u32(regs, 6) << '\n'
              << "event_boot_session_id: " << get_u32(regs, 8) << '\n'
              << "event_local_tick_ms: " << get_u64(regs, 10) << " ("
              << hex64(get_u64(regs, 10)) << ")\n"
              << "event_type: " << regs.at(14) << '\n'
              << "event_state: "
              << static_cast<unsigned>(
                     stm_protocol_event_state_from_word(regs.at(15)))
              << ", severity: "
              << static_cast<unsigned>(
                     stm_protocol_event_severity_from_word(regs.at(15)))
              << "\nevent_payload_word_count: " << regs.at(16) << '\n';
}

void decode_identity(const std::vector<std::uint16_t>& regs)
{
    const std::uint32_t capability = get_u32(regs, 2);
    std::cout << "protocol_version: "
              << static_cast<unsigned>(
                     stm_protocol_version_major(regs.at(0)))
              << '.'
              << static_cast<unsigned>(
                     stm_protocol_version_minor(regs.at(0)))
              << "\nfirmware_version: "
              << static_cast<unsigned>(
                     stm_protocol_version_major(regs.at(1)))
              << '.'
              << static_cast<unsigned>(
                     stm_protocol_version_minor(regs.at(1)))
              << "\ncapability_bitmap: " << hex32(capability)
              << "\n  IR=" << ((capability & STM_CAP_INFRARED_TEMPERATURE) != 0U)
              << ", ultrasonic="
              << ((capability & STM_CAP_ULTRASONIC) != 0U)
              << ", LED=" << ((capability & STM_CAP_LED) != 0U)
              << ", buzzer=" << ((capability & STM_CAP_BUZZER) != 0U)
              << ", display="
              << ((capability & STM_CAP_CHARACTER_DISPLAY) != 0U)
              << ", stage1="
              << ((capability & STM_CAP_STAGE1_SERVO) != 0U)
              << ", stage2=" << ((capability & STM_CAP_STAGE2_PUMP) != 0U)
              << "\nUID word0/1/2: " << hex32(get_u32(regs, 4)) << " | "
              << hex32(get_u32(regs, 6)) << " | "
              << hex32(get_u32(regs, 8))
              << "\nboot_session_id: " << get_u32(regs, 10)
              << "\nreset_reason: " << hex16(regs.at(12))
              << "\nhardware_revision: "
              << static_cast<unsigned>(
                     stm_protocol_version_major(regs.at(13)))
              << '.'
              << static_cast<unsigned>(
                     stm_protocol_version_minor(regs.at(13)))
              << '\n';
}

void decode_sensor(const std::vector<std::uint16_t>& regs)
{
    std::cout << "distance_mm: " << regs.at(0) << "mm\n"
              << "distance_age_ms: ";
    if (regs[1] == STM_PROTOCOL_DISTANCE_AGE_UNKNOWN) {
        std::cout << "unknown/stale\n";
    } else {
        std::cout << regs[1] << "ms\n";
    }
    const std::uint16_t flags = regs.at(2);
    std::cout << "sensor_status_flags: " << hex16(flags)
              << "\n  distance_valid="
              << ((flags & STM_SENSOR_DISTANCE_VALID) != 0U)
              << ", stale=" << ((flags & STM_SENSOR_DISTANCE_STALE) != 0U)
              << ", out_of_range="
              << ((flags & STM_SENSOR_DISTANCE_OUT_OF_RANGE) != 0U)
              << ", ultrasonic_fault="
              << ((flags & STM_SENSOR_ULTRASONIC_FAULT) != 0U)
              << ", infrared_fault="
              << ((flags & STM_SENSOR_INFRARED_FAULT) != 0U)
              << ", noisy=" << ((flags & STM_SENSOR_DISTANCE_NOISY) != 0U)
              << ", occupancy_tick_valid="
              << ((flags & STM_SENSOR_OCCUPANCY_TICK_VALID) != 0U)
              << "\noccupancy_changed_tick_ms: " << get_u64(regs, 3) << " ("
              << hex64(get_u64(regs, 3)) << ")\n";
}

void decode_window(const ReadWindow& window,
                   const std::vector<std::uint16_t>& regs)
{
    std::cout << "\n해석 결과\n";
    switch (window.key) {
        case '1':
            decode_base(regs);
            break;
        case '2':
            decode_result(regs);
            break;
        case '3':
            decode_event(regs);
            break;
        case '4':
            decode_identity(regs);
            break;
        case '5':
            decode_sensor(regs);
            break;
        default:
            break;
    }
}

std::optional<std::vector<std::uint16_t>> execute_read(
    const Options& options, p2::SerialPort& serial, const ReadWindow& window,
    bool ask_expected, bool show_schema = true)
{
    if (show_schema) {
        print_window_table(window);
    }
    const auto expected =
        ask_expected ? prompt_expectations(window.count)
                     : std::map<std::size_t, std::uint16_t>{};
    const std::vector<std::uint8_t> request = p2::build_read_input_request(
        options.slave, window.start, window.count);
    print_frame(request, false, window.start, window.count);
    if (options.dry_run) {
        std::cout << "\nDRY-RUN: serial 송신하지 않았습니다.\n";
        return std::nullopt;
    }
    const std::size_t expected_response_size =
        5U + static_cast<std::size_t>(window.count) * 2U;
    const p2::TransactionResult transaction = serial.transact(
        request, expected_response_size,
        std::chrono::milliseconds(options.timeout_ms));
    std::cout << "\n응답 시간: "
              << transaction.elapsed.count() / 1000.0 << "ms\n";
    if (!transaction.response.empty()) {
        std::cout << "RX HEX: " << p2::bytes_hex(transaction.response)
                  << "\nRX DEC: " << p2::bytes_decimal(transaction.response)
                  << '\n';
    }
    if (!transaction.ok) {
        std::cout << "통신 결과: FAIL - " << transaction.error << "\n";
        return std::nullopt;
    }
    const p2::ParsedResponse parsed = p2::parse_read_input_response(
        transaction.response, options.slave, window.count);
    if (!parsed.ok) {
        std::cout << "응답 검사: FAIL - " << parsed.error << '\n';
        return std::nullopt;
    }
    std::cout << "응답 검사: PASS (slave/FC/길이/CRC 정상)\n";
    print_register_values(window.start, parsed.registers, true);
    decode_window(window, parsed.registers);
    if (ask_expected) {
        compare_expectations(parsed.registers, expected);
    }
    return parsed.registers;
}

void print_command_table()
{
    std::cout
        << "\nFC16 command 선택\n"
        << "번호  opcode  이름               다음 argument와 의미\n"
        << "----  ------  -----------------  ------------------------------\n"
        << "1     0x0001  START_STAGE1       argument0/1=0, 방재포 동작 시작\n"
        << "2     0x0002  START_STAGE2       argument0/1=0, pump 진압 cycle 시작\n"
        << "3     0x0003  STOP_ACTUATOR      argument0 bit0=stage1, bit1=stage2\n"
        << "4     0x0004  REARM              argument0/1=0, 수동 점검 후 재무장\n"
        << "5     0x0010  ACK_EVENT          argument0/1=event sequence\n"
        << "6     0x0011  ACK_EVENT_STATUS   argument0/1=dropped event count\n"
        << "7     0x0012  ACK_RESTART        argument0/1=new boot session ID\n"
        << "8     0x0013  SYNC_TIME          argument0/1=Unix epoch seconds\n";
}

std::uint16_t command_from_choice(std::uint64_t choice)
{
    switch (choice) {
        case 1:
            return STM_COMMAND_START_STAGE1;
        case 2:
            return STM_COMMAND_START_STAGE2;
        case 3:
            return STM_COMMAND_STOP_ACTUATOR;
        case 4:
            return STM_COMMAND_REARM;
        case 5:
            return STM_COMMAND_ACK_EVENT;
        case 6:
            return STM_COMMAND_ACK_EVENT_STATUS;
        case 7:
            return STM_COMMAND_ACK_RESTART;
        case 8:
            return STM_COMMAND_SYNC_TIME;
        default:
            throw std::invalid_argument("command choice");
    }
}

void print_command_register_table(const std::vector<std::uint16_t>& regs)
{
    constexpr const char* names[] = {
        "command_opcode",   "command_id_hi", "command_id_lo",
        "origin_and_flags", "argument0",     "argument1",
        "reserved0",        "reserved1",
    };
    std::cout << "\nFC16 holding register 값\n"
              << "PDU addr  4xxxx  STM idx  variable             HEX     DEC\n"
              << "--------  -----  -------  -------------------  ------  -----\n";
    for (std::size_t i = 0; i < regs.size(); ++i) {
        const auto address = static_cast<std::uint16_t>(
            STM_HR_COMMAND_START + static_cast<std::uint16_t>(i));
        std::cout << std::setw(8) << hex16(address) << "  " << std::setw(5)
                  << 40001U + address << "  " << std::setw(7)
                  << stm_protocol_hr_array_index(address) << "  " << std::left
                  << std::setw(19) << names[i] << "  " << std::right
                  << std::setw(6) << hex16(regs[i]) << "  " << regs[i] << '\n';
    }
    print_register_values(STM_HR_COMMAND_START, regs, false);
}

void poll_command_result(const Options& options, p2::SerialPort& serial,
                         std::uint32_t command_id,
                         std::uint16_t expected_terminal)
{
    const ReadWindow& window = kWindows[1];
    const std::uint64_t timeout_seconds = prompt_integer(
        "결과 대기시간 seconds [기본 10]> ", 1, 600, 10);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            STM_PROTOCOL_BASE_POLL_PERIOD_MS));
        const auto regs = execute_read(options, serial, window, false, false);
        if (!regs.has_value()) {
            continue;
        }
        const std::uint32_t returned_id = get_u32(*regs, 0);
        const std::uint16_t status = regs->at(2);
        const bool id_match = returned_id == command_id;
        std::cout << "command ID 비교: expected=" << command_id
                  << ", actual=" << returned_id << " -> "
                  << (id_match ? "PASS" : "WAIT") << '\n';
        if (!id_match) {
            continue;
        }
        if (status == expected_terminal) {
            std::cout << "의도한 terminal status "
                      << command_status_name(expected_terminal)
                      << " 확인: PASS\n";
            return;
        }
        if (status == STM_COMMAND_STATUS_FAILED ||
            status == STM_COMMAND_STATUS_REJECTED) {
            std::cout << "의도한 결과와 다름: expected "
                      << command_status_name(expected_terminal) << ", actual "
                      << command_status_name(status) << " -> FAIL\n";
            return;
        }
        std::cout << "아직 terminal 상태가 아니므로 500ms 뒤 다시 확인합니다.\n";
    }
    std::cout << "결과 확인 timeout: 의도한 terminal status를 확인하지 못했습니다.\n";
}

void execute_write(const Options& options, p2::SerialPort& serial,
                   std::uint32_t& next_command_id)
{
    print_command_table();
    const std::uint64_t choice =
        prompt_integer("command 번호 1..8> ", 1, 8);
    const std::uint16_t opcode = command_from_choice(choice);
    const std::uint32_t command_id = static_cast<std::uint32_t>(
        prompt_integer("command_id (새 실행마다 증가, timeout retry는 동일 ID) [기본 " +
                           std::to_string(next_command_id) + "]> ",
                       1, std::numeric_limits<std::uint32_t>::max(),
                       next_command_id));
    next_command_id =
        command_id == std::numeric_limits<std::uint32_t>::max()
            ? 1U
            : command_id + 1U;
    std::cout << "\norigin 값\n"
              << "1 = QT_ADMIN: Qt에서 관리자가 승인한 명령\n"
              << "2 = PI_AUTOMATION: Pi 내부 자동화 명령\n";
    const std::uint16_t origin = static_cast<std::uint16_t>(
        prompt_integer("origin [기본 1]> ", 1, 2, 1));

    std::uint16_t argument0 = 0;
    std::uint16_t argument1 = 0;
    if (opcode == STM_COMMAND_STOP_ACTUATOR) {
        std::cout << "stage mask: 1=stage1, 2=stage2, 3=둘 다\n";
        argument0 =
            static_cast<std::uint16_t>(prompt_integer("stage mask> ", 1, 3));
    } else if (opcode == STM_COMMAND_SYNC_TIME) {
        const std::uint32_t epoch_seconds = static_cast<std::uint32_t>(
            prompt_integer(
                "Pi Unix epoch seconds (Pi NTP 동기화 확인, 기본은 현재 system clock)> ",
                0, std::numeric_limits<std::uint32_t>::max(),
                current_unix_epoch_seconds()));
        argument0 = stm_protocol_u32_hi(epoch_seconds);
        argument1 = stm_protocol_u32_lo(epoch_seconds);
        std::cout << "STM은 이 FC16 command 처리 시점의 local_tick_ms와 "
                     "epoch_seconds를 함께 저장해야 합니다.\n";
    } else if (opcode == STM_COMMAND_ACK_EVENT ||
               opcode == STM_COMMAND_ACK_EVENT_STATUS ||
               opcode == STM_COMMAND_ACK_RESTART) {
        const char* meaning =
            opcode == STM_COMMAND_ACK_EVENT
                ? "ACK할 event_sequence"
                : (opcode == STM_COMMAND_ACK_EVENT_STATUS
                       ? "확인한 dropped_event_count"
                       : "Pi가 새로 할당할 boot_session_id");
        const std::uint32_t value = static_cast<std::uint32_t>(
            prompt_integer(std::string(meaning) + " (uint32)> ", 0,
                           std::numeric_limits<std::uint32_t>::max()));
        argument0 = stm_protocol_u32_hi(value);
        argument1 = stm_protocol_u32_lo(value);
    }

    std::vector<std::uint16_t> regs(STM_HR_COMMAND_COUNT, 0);
    regs[STM_HR_COMMAND_OPCODE] = opcode;
    regs[STM_HR_COMMAND_ID_HI] = stm_protocol_u32_hi(command_id);
    regs[STM_HR_COMMAND_ID_LO] = stm_protocol_u32_lo(command_id);
    regs[STM_HR_COMMAND_ORIGIN_AND_FLAGS] =
        stm_protocol_pack_origin_and_flags(
            static_cast<std::uint8_t>(origin),
            STM_COMMAND_FLAGS_V0_1_NONE);
    regs[STM_HR_COMMAND_ARGUMENT0] = argument0;
    regs[STM_HR_COMMAND_ARGUMENT1] = argument1;

    print_command_register_table(regs);
    const std::vector<std::uint8_t> request =
        p2::build_write_multiple_request(options.slave, STM_HR_COMMAND_START,
                                         regs);
    print_frame(request, true, STM_HR_COMMAND_START, STM_HR_COMMAND_COUNT);
    if (options.dry_run) {
        std::cout << "\nDRY-RUN: serial 송신하지 않았습니다.\n";
        return;
    }
    if (opcode == STM_COMMAND_START_STAGE2) {
        std::cout
            << "\n주의: 이 명령은 실제 pump 진압 cycle을 시작할 수 있습니다.\n"
            << "사람·차량·전원·급수와 STM interlock 상태를 먼저 확인하십시오.\n";
        if (prompt("계속하려면 STAGE2를 정확히 입력> ") != "STAGE2") {
            std::cout << "stage 2 송신을 취소했습니다.\n";
            return;
        }
    }
    if (!prompt_yes_no("이 frame을 실제 송신합니까? [y/N]> ", false)) {
        std::cout << "송신을 취소했습니다.\n";
        return;
    }
    const p2::TransactionResult transaction = serial.transact(
        request, 8, std::chrono::milliseconds(options.timeout_ms));
    std::cout << "\nFC16 응답 시간: "
              << transaction.elapsed.count() / 1000.0 << "ms\n";
    if (!transaction.response.empty()) {
        std::cout << "RX HEX: " << p2::bytes_hex(transaction.response)
                  << "\nRX DEC: " << p2::bytes_decimal(transaction.response)
                  << '\n';
    }
    if (!transaction.ok) {
        std::cout << "통신 결과: FAIL - " << transaction.error << '\n';
        return;
    }
    const p2::ParsedResponse parsed = p2::parse_write_multiple_response(
        transaction.response, options.slave, STM_HR_COMMAND_START,
        STM_HR_COMMAND_COUNT);
    if (!parsed.ok) {
        std::cout << "FC16 응답 검사: FAIL - " << parsed.error << '\n';
        return;
    }
    std::cout << "FC16 응답 검사: PASS\n"
              << "주의: 이는 STM register write 접수 확인이며 actuator 완료가 아닙니다.\n";

    if (!prompt_yes_no("Command result를 500ms 간격으로 확인합니까? [Y/n]> ",
                       true)) {
        return;
    }
    std::cout << "\n의도한 terminal status\n"
              << "3=COMPLETED, 4=FAILED, 5=REJECTED\n";
    const std::uint16_t expected_terminal = static_cast<std::uint16_t>(
        prompt_integer("expected status [기본 3=COMPLETED]> ", 3, 5, 3));
    poll_command_result(options, serial, command_id, expected_terminal);
}

void print_read_menu()
{
    std::cout << "\nFC4 read 선택\n"
              << "1 Base state      0x0000, 6 regs, 매 scan 온도·화재·점유\n"
              << "2 Command result  0x0010, 6 regs, 명령 진행·완료\n"
              << "3 Event status    0x0020, 21 regs, pending event 상세\n"
              << "4 Identity        0x0040, 16 regs, version·UID·capability\n"
              << "5 Sensor detail   0x0060, 8 regs, distance·점유 변경 tick\n";
}

const ReadWindow* find_window(char key)
{
    for (const ReadWindow& window : kWindows) {
        if (window.key == key) {
            return &window;
        }
    }
    return nullptr;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(arg + " 뒤에 값이 필요합니다.");
            }
            return argv[++i];
        };
        if (arg == "--device") {
            options.device = next();
        } else if (arg == "--slave") {
            options.slave = static_cast<std::uint8_t>(
                parse_integer(next(), 1, 247));
        } else if (arg == "--timeout-ms") {
            options.timeout_ms =
                static_cast<int>(parse_integer(next(), 1, 5000));
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--kernel-rs485") {
            options.kernel_rs485 = true;
        } else if (arg == "--self-test") {
            options.self_test = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: p2-stm-modbus-test [options]\n"
                << "  --device PATH       기본 /dev/ttyUSB0\n"
                << "  --slave 1..247      기본 1\n"
                << "  --timeout-ms N      기본 30\n"
                << "  --kernel-rs485      UART RTS kernel direction control\n"
                << "  --dry-run           frame 생성만 하고 송신하지 않음\n"
                << "  --self-test         기본 codec smoke test\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("알 수 없는 option: " + arg);
        }
    }
    return options;
}

int run_self_test()
{
    const auto read =
        p2::build_read_input_request(1, STM_IR_BASE_STATE_START,
                                     STM_IR_BASE_STATE_COUNT);
    const auto write = p2::build_write_multiple_request(
        1, STM_HR_COMMAND_START,
        std::vector<std::uint16_t>(STM_HR_COMMAND_COUNT, 0));
    const bool ok = read.size() == 8 && p2::has_valid_modbus_crc(read) &&
                    write.size() == 25 && p2::has_valid_modbus_crc(write) &&
                    stm_protocol_hr_array_index(STM_HR_COMMAND_START) == 0;
    std::cout << "self-test: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        if (options.self_test) {
            return run_self_test();
        }

        std::cout
            << "P2.1 STM Modbus RTU 개발 시험기\n"
            << "UART=19200 8E1, slave="
            << static_cast<unsigned>(options.slave)
            << ", timeout=" << options.timeout_ms << "ms\n"
            << "device=" << options.device
            << (options.dry_run ? " [DRY-RUN]\n" : "\n")
            << "PDU logical address가 실제 wire address입니다. 3xxxx/4xxxx는 "
               "사람용 reference입니다.\n";

        p2::SerialPort serial;
        if (!options.dry_run) {
            std::string error;
            if (!serial.open_port(options.device, options.kernel_rs485,
                                  error)) {
                std::cerr << "serial 준비 실패: " << error << '\n';
                return 2;
            }
        }

        std::uint32_t next_command_id = 1;
        while (true) {
            std::cout << "\n동작 선택\n"
                      << "R = FC4 read\n"
                      << "W = FC16 command write\n"
                      << "Q = 종료\n";
            const std::string action = prompt("선택> ");
            if (action == "q" || action == "Q") {
                break;
            }
            if (action == "r" || action == "R") {
                print_read_menu();
                const std::string choice = prompt("read 번호 1..5> ");
                if (choice.size() != 1 || find_window(choice[0]) == nullptr) {
                    std::cout << "잘못된 read 선택입니다.\n";
                    continue;
                }
                execute_read(options, serial, *find_window(choice[0]), true);
                continue;
            }
            if (action == "w" || action == "W") {
                execute_write(options, serial, next_command_id);
                continue;
            }
            std::cout << "R, W 또는 Q를 입력하십시오.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "오류: " << error.what() << '\n';
        return 1;
    }
}
