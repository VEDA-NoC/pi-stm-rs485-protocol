#include "stm_protocol.h"

#if defined(__cplusplus)
#define STM_TEST_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define STM_TEST_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

STM_TEST_STATIC_ASSERT(STM_PROTOCOL_UART_BITS_PER_CHARACTER == 11,
                       "8E1 must use 11 wire bits per character");
STM_TEST_STATIC_ASSERT(STM_PROTOCOL_VERSION_MINOR == 2,
                       "NTP time sync is protocol minor version 2");
STM_TEST_STATIC_ASSERT(STM_PROTOCOL_TEMPERATURE_UNITS_PER_CELSIUS == 100,
                       "temperature scale must be 0.01 degree Celsius");
STM_TEST_STATIC_ASSERT(STM_PROTOCOL_DISTANCE_UNIT_MM == 1,
                       "distance unit must be one millimetre");
STM_TEST_STATIC_ASSERT(STM_CAP_V0_1_KNOWN_MASK == UINT32_C(0x0000007f),
                       "capability bits 0..6 must be defined");
STM_TEST_STATIC_ASSERT(
    STM_IR_BASE_STATE_START + STM_IR_BASE_STATE_COUNT <=
        STM_IR_COMMAND_RESULT_START,
    "base state and command result windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_IR_COMMAND_RESULT_START + STM_IR_COMMAND_RESULT_COUNT <=
        STM_IR_EVENT_STATUS_START,
    "command result and event status windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_IR_EVENT_STATUS_START + STM_IR_EVENT_STATUS_RESERVED_COUNT <=
        STM_IR_IDENTITY_START,
    "event status and identity windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_IR_IDENTITY_START + STM_IR_IDENTITY_COUNT <=
        STM_IR_SENSOR_DETAIL_START,
    "identity and sensor detail windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_IR_SENSOR_DETAIL_START + STM_IR_SENSOR_DETAIL_COUNT <=
        STM_IR_DIAGNOSTIC_START,
    "sensor detail and diagnostic windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_IR_DIAGNOSTIC_START + STM_IR_DIAGNOSTIC_RESERVED_COUNT <=
        STM_IR_REGION_START + STM_IR_REGION_COUNT,
    "input register region does not contain all FC4 windows");
STM_TEST_STATIC_ASSERT(
    STM_HR_COMMAND_START + STM_HR_COMMAND_COUNT <= STM_HR_CONFIG_START,
    "command and configuration windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_HR_CONFIG_START + STM_HR_CONFIG_RESERVED_COUNT <=
        STM_HR_COMMISSIONING_START,
    "configuration and commissioning windows overlap");
STM_TEST_STATIC_ASSERT(
    STM_HR_COMMISSIONING_START + STM_HR_COMMISSIONING_RESERVED_COUNT <=
        STM_HR_REGION_START + STM_HR_REGION_COUNT,
    "holding register region does not contain all FC16 windows");
STM_TEST_STATIC_ASSERT(STM_IR_EVENT_SLOT_PAYLOAD3 + 1 ==
                           STM_IR_EVENT_STATUS_USED_COUNT,
                       "event window used count is inconsistent");
STM_TEST_STATIC_ASSERT(
    (STM_STATE_RESERVED_MASK &
     (STM_STATE_TEMPERATURE_VALID | STM_STATE_TEMPERATURE_STALE |
      STM_STATE_OCCUPANCY_VALID | STM_STATE_OCCUPIED |
      STM_STATE_FIRE_ACTIVE | STM_STATE_EVENT_PENDING |
      STM_STATE_EVENT_OVERFLOW | STM_STATE_SENSOR_FAULT |
      STM_STATE_ACTUATOR_FAULT | STM_STATE_DEVICE_RESTARTED |
      STM_STATE_REARM_REQUIRED)) == 0,
    "state reserved mask overlaps a defined state bit");
STM_TEST_STATIC_ASSERT(
    (STM_SENSOR_STATUS_RESERVED_MASK &
     (STM_SENSOR_DISTANCE_VALID | STM_SENSOR_DISTANCE_STALE |
      STM_SENSOR_DISTANCE_OUT_OF_RANGE | STM_SENSOR_ULTRASONIC_FAULT |
      STM_SENSOR_INFRARED_FAULT | STM_SENSOR_DISTANCE_NOISY |
      STM_SENSOR_OCCUPANCY_TICK_VALID)) == 0,
    "sensor reserved mask overlaps a defined sensor bit");
STM_TEST_STATIC_ASSERT(
    (STM_RESET_RESERVED_MASK &
     (STM_RESET_POWER_ON | STM_RESET_EXTERNAL_PIN | STM_RESET_SOFTWARE |
      STM_RESET_WATCHDOG | STM_RESET_BROWNOUT | STM_RESET_LOW_POWER |
      STM_RESET_UNKNOWN)) == 0,
    "reset reserved mask overlaps a defined reset bit");

static int test_u32_round_trip(void)
{
    const uint32_t source = UINT32_C(0x12345678);
    return stm_protocol_u32_from_regs(stm_protocol_u32_hi(source),
                                      stm_protocol_u32_lo(source)) == source;
}

static int test_u64_round_trip(void)
{
    const uint64_t source = UINT64_C(0x0123456789abcdef);
    uint16_t registers[4] = {0, 0, 0, 0};

    stm_protocol_u64_to_regs(source, registers);
    return registers[0] == UINT16_C(0x0123) &&
           registers[1] == UINT16_C(0x4567) &&
           registers[2] == UINT16_C(0x89ab) &&
           registers[3] == UINT16_C(0xcdef) &&
           stm_protocol_u64_from_regs(registers) == source;
}

static int test_packed_fields(void)
{
    const uint16_t actuator = stm_protocol_pack_actuator_status(
        STM_ACTUATOR_RUNNING, STM_ACTUATOR_COMPLETE);
    const uint16_t origin_and_flags = stm_protocol_pack_origin_and_flags(
        STM_COMMAND_ORIGIN_QT_ADMIN, UINT8_C(0xa5));
    const uint16_t event_state_and_severity =
        stm_protocol_pack_event_state_and_severity(
            STM_EVENT_STATE_STARTED, STM_EVENT_SEVERITY_CRITICAL);

    return stm_protocol_stage1_from_status(actuator) == STM_ACTUATOR_RUNNING &&
           stm_protocol_stage2_from_status(actuator) == STM_ACTUATOR_COMPLETE &&
           stm_protocol_origin_from_word(origin_and_flags) ==
               STM_COMMAND_ORIGIN_QT_ADMIN &&
           stm_protocol_flags_from_word(origin_and_flags) == UINT8_C(0xa5) &&
           stm_protocol_event_state_from_word(event_state_and_severity) ==
               STM_EVENT_STATE_STARTED &&
           stm_protocol_event_severity_from_word(event_state_and_severity) ==
               STM_EVENT_SEVERITY_CRITICAL;
}

static int test_region_indexes(void)
{
    return stm_protocol_ir_array_index(STM_IR_BASE_STATE_START) == 0 &&
           stm_protocol_ir_array_index(STM_IR_SENSOR_DETAIL_START) ==
               UINT16_C(0x0060) &&
           stm_protocol_hr_array_index(STM_HR_COMMAND_START) == 0 &&
           stm_protocol_hr_array_index(STM_HR_COMMISSIONING_START) ==
               UINT16_C(0x0040);
}

int main(void)
{
    return (test_u32_round_trip() && test_u64_round_trip() &&
            test_packed_fields() && test_region_indexes())
               ? 0
               : 1;
}
