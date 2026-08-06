#ifndef STM_PROTOCOL_H
#define STM_PROTOCOL_H

/*
 * P2.1 STM32 <-> Raspberry Pi Modbus RTU protocol draft v0.1
 *
 * - Wire framing/CRC는 Modbus library가 담당한다.
 * - 이 header는 16-bit Modbus register 주소와 field 의미를 공유한다.
 * - packed struct의 메모리를 UART로 직접 보내지 않는다.
 * - multi-register integer는 high register first다.
 * - enum 선언은 symbolic value용이며 wire에는 uint16_t로 encode한다.
 *
 * Draft: 팀 검토 전 구현 계약으로 확정하지 않는다.
 */

#include <stdint.h>

#define STM_PROTOCOL_VERSION_MAJOR UINT8_C(0)
#define STM_PROTOCOL_VERSION_MINOR UINT8_C(2)

/* Wire UART: 1 start + 8 data + even parity + 1 stop = 11 bits/character. */
#define STM_PROTOCOL_BAUD_RATE UINT32_C(19200)
#define STM_PROTOCOL_UART_DATA_BITS UINT8_C(8)
#define STM_PROTOCOL_UART_STOP_BITS UINT8_C(1)
#define STM_PROTOCOL_UART_BITS_PER_CHARACTER UINT8_C(11)

/* Shared application units. */
#define STM_PROTOCOL_TEMPERATURE_UNITS_PER_CELSIUS UINT16_C(100)
#define STM_PROTOCOL_DISTANCE_UNIT_MM UINT16_C(1)
#define STM_PROTOCOL_LOCAL_TIME_UNIT_MS UINT16_C(1)

/*
 * STM32F401 CubeMX/HAL 설정 주의:
 * parity가 peripheral word length에 포함되므로 wire 8E1은
 * 9-bit word length + even parity + 1 stop bit로 설정한다.
 */
#define STM_PROTOCOL_STM32_UART_WORD_LENGTH_BITS UINT8_C(9)

#define STM_PROTOCOL_FC_READ_INPUT_REGISTERS UINT8_C(4)
#define STM_PROTOCOL_FC_WRITE_MULTIPLE_REGISTERS UINT8_C(16)
#define STM_PROTOCOL_FC_FUTURE_DISCOVERY UINT8_C(0x41)

#define STM_PROTOCOL_MAX_NODE_COUNT UINT8_C(20)
#define STM_PROTOCOL_COMMISSIONING_ADDRESS UINT8_C(247)
#define STM_PROTOCOL_FIRST_ASSIGNED_ADDRESS UINT8_C(1)
#define STM_PROTOCOL_LAST_ASSIGNED_ADDRESS UINT8_C(246)

/*
 * Base-state scheduler.
 *
 * - 같은 STM의 base poll 시작 간격은 최소 500ms다.
 * - 정상 20-node sweep planning 값은 약 447ms다.
 * - 오류/OS scheduling을 포함한 cycle budget은 700ms다.
 * - base sweep 도중 같은 node를 즉시 재시도하지 않는다.
 * - event drain은 base sweep 뒤 node당 최대 1건이다.
 */
#define STM_PROTOCOL_BASE_POLL_PERIOD_MS UINT32_C(500)
#define STM_PROTOCOL_SWEEP_BUDGET_MS UINT32_C(700)
#define STM_PROTOCOL_RESPONSE_TIMEOUT_MS UINT32_C(30)
#define STM_PROTOCOL_OFFLINE_PROBE_PERIOD_MS UINT32_C(1000)
#define STM_PROTOCOL_INLINE_RETRY_COUNT UINT8_C(0)
#define STM_PROTOCOL_EVENT_DRAIN_PER_NODE_PER_SWEEP UINT8_C(1)

/* FC4 input-register windows. */
enum {
    STM_IR_REGION_START = 0x0000,
    STM_IR_REGION_COUNT = 0x00a0,

    STM_IR_BASE_STATE_START = 0x0000,
    STM_IR_BASE_STATE_COUNT = 6,

    STM_IR_COMMAND_RESULT_START = 0x0010,
    STM_IR_COMMAND_RESULT_COUNT = 6,

    STM_IR_EVENT_STATUS_START = 0x0020,
    STM_IR_EVENT_STATUS_USED_COUNT = 21,
    STM_IR_EVENT_STATUS_RESERVED_COUNT = 32,

    STM_IR_IDENTITY_START = 0x0040,
    STM_IR_IDENTITY_COUNT = 16,

    STM_IR_SENSOR_DETAIL_START = 0x0060,
    STM_IR_SENSOR_DETAIL_COUNT = 8,

    STM_IR_DIAGNOSTIC_START = 0x0080,
    STM_IR_DIAGNOSTIC_RESERVED_COUNT = 32
};

/* FC4 base-state offsets relative to STM_IR_BASE_STATE_START. */
enum {
    STM_IR_BASE_TEMPERATURE_CENTI_C = 0,
    STM_IR_BASE_STATE_FLAGS = 1,
    STM_IR_BASE_TEMPERATURE_AGE_MS = 2,
    STM_IR_BASE_ACTUATOR_STATUS = 3,
    STM_IR_BASE_OLDEST_EVENT_SEQUENCE_HI = 4,
    STM_IR_BASE_OLDEST_EVENT_SEQUENCE_LO = 5
};

/*
 * temperature_centi_c:
 *   45.12°C -> 4512. 모든 uint16_t 값은 온도 데이터로 사용할 수 있다.
 *
 * temperature_age_ms:
 *   측정 뒤 경과시간 0..65534ms. 0xffff는 unknown/stale이다.
 *
 * oldest_pending_event_sequence:
 *   ACK되지 않은 가장 오래된 event의 uint32_t sequence. queue가 비면 0이다.
 */
#define STM_PROTOCOL_SAMPLE_AGE_UNKNOWN UINT16_C(0xffff)
#define STM_PROTOCOL_SAMPLE_AGE_MAX_MS UINT16_C(0xfffe)
#define STM_PROTOCOL_TEMPERATURE_AGE_UNKNOWN STM_PROTOCOL_SAMPLE_AGE_UNKNOWN
#define STM_PROTOCOL_TEMPERATURE_AGE_MAX_MS STM_PROTOCOL_SAMPLE_AGE_MAX_MS
#define STM_PROTOCOL_DISTANCE_AGE_UNKNOWN STM_PROTOCOL_SAMPLE_AGE_UNKNOWN
#define STM_PROTOCOL_DISTANCE_AGE_MAX_MS STM_PROTOCOL_SAMPLE_AGE_MAX_MS
#define STM_PROTOCOL_EVENT_SEQUENCE_NONE UINT32_C(0)

/* STM_IR_BASE_STATE_FLAGS bits. */
#define STM_STATE_TEMPERATURE_VALID (UINT16_C(1) << 0)
#define STM_STATE_TEMPERATURE_STALE (UINT16_C(1) << 1)
#define STM_STATE_OCCUPANCY_VALID (UINT16_C(1) << 2)
#define STM_STATE_OCCUPIED (UINT16_C(1) << 3)
#define STM_STATE_FIRE_ACTIVE (UINT16_C(1) << 4)
#define STM_STATE_EVENT_PENDING (UINT16_C(1) << 5)
#define STM_STATE_EVENT_OVERFLOW (UINT16_C(1) << 6)
#define STM_STATE_SENSOR_FAULT (UINT16_C(1) << 7)
#define STM_STATE_ACTUATOR_FAULT (UINT16_C(1) << 8)
#define STM_STATE_DEVICE_RESTARTED (UINT16_C(1) << 9)
#define STM_STATE_REARM_REQUIRED (UINT16_C(1) << 10)
#define STM_STATE_RESERVED_MASK UINT16_C(0xf800)

/*
 * actuator_status:
 *   low byte  = stage 1 state
 *   high byte = stage 2 state
 */
typedef enum {
    STM_ACTUATOR_NOT_PRESENT = 0,
    STM_ACTUATOR_IDLE = 1,
    STM_ACTUATOR_RUNNING = 2,
    STM_ACTUATOR_COMPLETE = 3,
    STM_ACTUATOR_FAILED = 4,
    STM_ACTUATOR_STOPPED = 5,
    STM_ACTUATOR_INTERLOCKED = 6,
    STM_ACTUATOR_REARM_REQUIRED = 7
} stm_actuator_state_t;

/* FC4 command-result offsets relative to STM_IR_COMMAND_RESULT_START. */
enum {
    STM_IR_RESULT_COMMAND_ID_HI = 0,
    STM_IR_RESULT_COMMAND_ID_LO = 1,
    STM_IR_RESULT_STATUS = 2,
    STM_IR_RESULT_REASON = 3,
    STM_IR_RESULT_ACTUATOR_STATUS = 4,
    STM_IR_RESULT_RESERVED = 5
};

typedef enum {
    STM_COMMAND_STATUS_NONE = 0,
    STM_COMMAND_STATUS_ACCEPTED = 1,
    STM_COMMAND_STATUS_RUNNING = 2,
    STM_COMMAND_STATUS_COMPLETED = 3,
    STM_COMMAND_STATUS_FAILED = 4,
    STM_COMMAND_STATUS_REJECTED = 5
} stm_command_status_t;

typedef enum {
    STM_COMMAND_REASON_NONE = 0,
    STM_COMMAND_REASON_UNSUPPORTED = 1,
    STM_COMMAND_REASON_INVALID_ARGUMENT = 2,
    STM_COMMAND_REASON_BUSY = 3,
    STM_COMMAND_REASON_INTERLOCK = 4,
    STM_COMMAND_REASON_FIRE_STILL_ACTIVE = 5,
    STM_COMMAND_REASON_REARM_REQUIRED = 6,
    STM_COMMAND_REASON_LOCAL_FAULT = 7,
    STM_COMMAND_REASON_ID_CONFLICT = 8
} stm_command_reason_t;

/* FC4 event-status offsets relative to STM_IR_EVENT_STATUS_START. */
enum {
    STM_IR_EVENT_PENDING_COUNT = 0,
    STM_IR_EVENT_STATUS_FLAGS = 1,
    STM_IR_EVENT_DROPPED_COUNT_HI = 2,
    STM_IR_EVENT_DROPPED_COUNT_LO = 3,
    STM_IR_EVENT_OLDEST_AVAILABLE_SEQUENCE_HI = 4,
    STM_IR_EVENT_OLDEST_AVAILABLE_SEQUENCE_LO = 5,

    STM_IR_EVENT_SLOT_SEQUENCE_HI = 6,
    STM_IR_EVENT_SLOT_SEQUENCE_LO = 7,
    STM_IR_EVENT_SLOT_BOOT_SESSION_HI = 8,
    STM_IR_EVENT_SLOT_BOOT_SESSION_LO = 9,
    STM_IR_EVENT_SLOT_LOCAL_TICK_WORD3 = 10,
    STM_IR_EVENT_SLOT_LOCAL_TICK_WORD2 = 11,
    STM_IR_EVENT_SLOT_LOCAL_TICK_WORD1 = 12,
    STM_IR_EVENT_SLOT_LOCAL_TICK_WORD0 = 13,
    STM_IR_EVENT_SLOT_TYPE = 14,
    STM_IR_EVENT_SLOT_STATE_AND_SEVERITY = 15,
    STM_IR_EVENT_SLOT_PAYLOAD_WORD_COUNT = 16,
    STM_IR_EVENT_SLOT_PAYLOAD0 = 17,
    STM_IR_EVENT_SLOT_PAYLOAD1 = 18,
    STM_IR_EVENT_SLOT_PAYLOAD2 = 19,
    STM_IR_EVENT_SLOT_PAYLOAD3 = 20
};

#define STM_PROTOCOL_EVENT_PAYLOAD_MAX_REGISTERS UINT16_C(4)
#define STM_PROTOCOL_EVENT_PAYLOAD_MAX_BYTES UINT16_C(8)

#define STM_EVENT_STATUS_OVERFLOW_LATCHED (UINT16_C(1) << 0)
#define STM_EVENT_STATUS_CRITICAL_OVERFLOW (UINT16_C(1) << 1)
#define STM_EVENT_STATUS_NORMAL_OVERFLOW (UINT16_C(1) << 2)
#define STM_EVENT_STATUS_RESERVED_MASK UINT16_C(0xfff8)

typedef enum {
    STM_EVENT_NONE = 0,
    STM_EVENT_FIRE_STARTED = 1,
    STM_EVENT_FIRE_CLEARED = 2,
    STM_EVENT_OCCUPANCY_CHANGED = 3,
    STM_EVENT_STAGE1_STATE_CHANGED = 4,
    STM_EVENT_STAGE2_STATE_CHANGED = 5,
    STM_EVENT_SENSOR_FAULT = 6,
    STM_EVENT_ACTUATOR_FAULT = 7,
    STM_EVENT_DEVICE_RESTARTED = 8,
    STM_EVENT_QUEUE_OVERFLOW = 9
} stm_event_type_t;

typedef enum {
    STM_EVENT_STATE_OCCURRED = 0,
    STM_EVENT_STATE_STARTED = 1,
    STM_EVENT_STATE_ENDED = 2
} stm_event_state_t;

typedef enum {
    STM_EVENT_SEVERITY_INFO = 0,
    STM_EVENT_SEVERITY_WARNING = 1,
    STM_EVENT_SEVERITY_CRITICAL = 2
} stm_event_severity_t;

/*
 * event_state_and_severity:
 *   low byte  = stm_event_state_t
 *   high byte = stm_event_severity_t
 */

/*
 * v0.1 event payload schemas. All unused payload registers are zero.
 *
 * FIRE_STARTED/CLEARED:
 *   payload0 = temperature_centi_c, payload1 = state_flags
 * OCCUPANCY_CHANGED:
 *   payload0 = occupied (0/1), payload1 = distance_mm
 * STAGE1/2_STATE_CHANGED:
 *   payload0 = actuator_status, payload1 = command reason or 0
 * SENSOR_FAULT:
 *   payload0 = sensor_status_flags, payload1 = implementation diagnostic code
 * ACTUATOR_FAULT:
 *   payload0 = actuator_status, payload1 = implementation diagnostic code
 * DEVICE_RESTARTED:
 *   payload0 = reset_reason
 * QUEUE_OVERFLOW:
 *   payload0/1 = dropped_event_count high/low
 */
#define STM_EVENT_PAYLOAD_WORDS_FIRE UINT16_C(2)
#define STM_EVENT_PAYLOAD_WORDS_OCCUPANCY UINT16_C(2)
#define STM_EVENT_PAYLOAD_WORDS_ACTUATOR_STATE UINT16_C(2)
#define STM_EVENT_PAYLOAD_WORDS_SENSOR_FAULT UINT16_C(2)
#define STM_EVENT_PAYLOAD_WORDS_ACTUATOR_FAULT UINT16_C(2)
#define STM_EVENT_PAYLOAD_WORDS_DEVICE_RESTARTED UINT16_C(1)
#define STM_EVENT_PAYLOAD_WORDS_QUEUE_OVERFLOW UINT16_C(2)

/* FC4 identity offsets relative to STM_IR_IDENTITY_START. */
enum {
    STM_IR_ID_PROTOCOL_VERSION = 0,
    STM_IR_ID_FIRMWARE_VERSION = 1,
    STM_IR_ID_CAPABILITY_HI = 2,
    STM_IR_ID_CAPABILITY_LO = 3,
    STM_IR_ID_UID_WORD0_HI = 4,
    STM_IR_ID_UID_WORD0_LO = 5,
    STM_IR_ID_UID_WORD1_HI = 6,
    STM_IR_ID_UID_WORD1_LO = 7,
    STM_IR_ID_UID_WORD2_HI = 8,
    STM_IR_ID_UID_WORD2_LO = 9,
    STM_IR_ID_BOOT_SESSION_HI = 10,
    STM_IR_ID_BOOT_SESSION_LO = 11,
    STM_IR_ID_RESET_REASON = 12,
    STM_IR_ID_HARDWARE_REVISION = 13,
    STM_IR_ID_RESERVED0 = 14,
    STM_IR_ID_RESERVED1 = 15
};

/* capability_bitmap: installed hardware, not current health. */
#define STM_CAP_INFRARED_TEMPERATURE (UINT32_C(1) << 0)
#define STM_CAP_ULTRASONIC (UINT32_C(1) << 1)
#define STM_CAP_LED (UINT32_C(1) << 2)
#define STM_CAP_BUZZER (UINT32_C(1) << 3)
#define STM_CAP_CHARACTER_DISPLAY (UINT32_C(1) << 4)
#define STM_CAP_STAGE1_SERVO (UINT32_C(1) << 5)
#define STM_CAP_STAGE2_PUMP (UINT32_C(1) << 6)
#define STM_CAP_V0_1_KNOWN_MASK UINT32_C(0x0000007f)

/* reset_reason is a bitmap because more than one RCC_CSR flag may be set. */
#define STM_RESET_POWER_ON (UINT16_C(1) << 0)
#define STM_RESET_EXTERNAL_PIN (UINT16_C(1) << 1)
#define STM_RESET_SOFTWARE (UINT16_C(1) << 2)
#define STM_RESET_WATCHDOG (UINT16_C(1) << 3)
#define STM_RESET_BROWNOUT (UINT16_C(1) << 4)
#define STM_RESET_LOW_POWER (UINT16_C(1) << 5)
#define STM_RESET_UNKNOWN (UINT16_C(1) << 15)
#define STM_RESET_RESERVED_MASK UINT16_C(0x7fc0)

/* FC4 sensor-detail offsets relative to STM_IR_SENSOR_DETAIL_START. */
enum {
    STM_IR_SENSOR_DISTANCE_MM = 0,
    STM_IR_SENSOR_DISTANCE_AGE_MS = 1,
    STM_IR_SENSOR_STATUS_FLAGS = 2,
    STM_IR_SENSOR_OCCUPANCY_CHANGED_TICK_WORD3 = 3,
    STM_IR_SENSOR_OCCUPANCY_CHANGED_TICK_WORD2 = 4,
    STM_IR_SENSOR_OCCUPANCY_CHANGED_TICK_WORD1 = 5,
    STM_IR_SENSOR_OCCUPANCY_CHANGED_TICK_WORD0 = 6,
    STM_IR_SENSOR_RESERVED = 7
};

#define STM_SENSOR_DISTANCE_VALID (UINT16_C(1) << 0)
#define STM_SENSOR_DISTANCE_STALE (UINT16_C(1) << 1)
#define STM_SENSOR_DISTANCE_OUT_OF_RANGE (UINT16_C(1) << 2)
#define STM_SENSOR_ULTRASONIC_FAULT (UINT16_C(1) << 3)
#define STM_SENSOR_INFRARED_FAULT (UINT16_C(1) << 4)
#define STM_SENSOR_DISTANCE_NOISY (UINT16_C(1) << 5)
#define STM_SENSOR_OCCUPANCY_TICK_VALID (UINT16_C(1) << 6)
#define STM_SENSOR_STATUS_RESERVED_MASK UINT16_C(0xff80)

/* FC16 command mailbox. */
enum {
    STM_HR_REGION_START = 0x0100,
    STM_HR_REGION_COUNT = 0x0060,

    STM_HR_COMMAND_START = 0x0100,
    STM_HR_COMMAND_COUNT = 8,

    STM_HR_COMMAND_OPCODE = 0,
    STM_HR_COMMAND_ID_HI = 1,
    STM_HR_COMMAND_ID_LO = 2,
    STM_HR_COMMAND_ORIGIN_AND_FLAGS = 3,
    STM_HR_COMMAND_ARGUMENT0 = 4,
    STM_HR_COMMAND_ARGUMENT1 = 5,
    STM_HR_COMMAND_RESERVED0 = 6,
    STM_HR_COMMAND_RESERVED1 = 7,

    STM_HR_CONFIG_START = 0x0120,
    STM_HR_CONFIG_RESERVED_COUNT = 32,

    STM_HR_COMMISSIONING_START = 0x0140,
    STM_HR_COMMISSIONING_RESERVED_COUNT = 32
};

/*
 * command_opcode = 수행할 동작 종류.
 * command_id     = 이번 실행 건의 uint32_t 식별자.
 *
 * timeout 재전송은 같은 command_id와 같은 payload를 사용한다. STM은 같은 ID를
 * 다시 실행하지 않고 기존 result를 반환한다. 같은 ID에 다른 payload가 오면
 * REJECTED/ID_CONFLICT다. mailbox reserved register는 v0.1에서 반드시 0이다.
 */
typedef enum {
    STM_COMMAND_NONE = 0x0000,
    STM_COMMAND_START_STAGE1 = 0x0001,
    STM_COMMAND_START_STAGE2 = 0x0002,
    STM_COMMAND_STOP_ACTUATOR = 0x0003,
    STM_COMMAND_REARM = 0x0004,

    STM_COMMAND_ACK_EVENT = 0x0010,
    STM_COMMAND_ACK_EVENT_STATUS = 0x0011,
    STM_COMMAND_ACK_RESTART = 0x0012,
    STM_COMMAND_SYNC_TIME = 0x0013,

    STM_COMMAND_WRITE_CONFIG_COMMIT = 0x0020
} stm_command_opcode_t;

typedef enum {
    STM_COMMAND_ORIGIN_UNKNOWN = 0,
    STM_COMMAND_ORIGIN_QT_ADMIN = 1,
    STM_COMMAND_ORIGIN_PI_AUTOMATION = 2
} stm_command_origin_t;

/*
 * origin_and_flags:
 *   low byte  = stm_command_origin_t
 *   high byte = command flags; v0.1은 0
 */
#define STM_COMMAND_FLAGS_V0_1_NONE UINT8_C(0)

/*
 * opcode별 argument:
 * - START_STAGE1/2, REARM: argument0/1 = 0
 * - STOP_ACTUATOR: argument0 = stage mask, argument1 = 0
 * - ACK_EVENT: argument0/1 = event_sequence high/low
 * - ACK_EVENT_STATUS: argument0/1 = 확인한 dropped_event_count high/low
 * - ACK_RESTART: argument0/1 = Pi가 할당한 boot_session_id high/low
 * - SYNC_TIME: argument0/1 = Pi의 Unix epoch seconds high/low
 *
 * SYNC_TIME을 처리한 STM은 command를 수신한 순간의 local monotonic tick과 epoch
 * seconds를 RAM에 함께 저장한다. STM은 자체 NTP client가 아니며, Pi가 NTP로
 * 동기화된 system clock일 때만 이 command를 보내야 한다.
 */
#define STM_STOP_STAGE1_MASK (UINT16_C(1) << 0)
#define STM_STOP_STAGE2_MASK (UINT16_C(1) << 1)

static inline uint16_t stm_protocol_ir_array_index(uint16_t pdu_address)
{
    return (uint16_t)(pdu_address - STM_IR_REGION_START);
}

static inline uint16_t stm_protocol_hr_array_index(uint16_t pdu_address)
{
    return (uint16_t)(pdu_address - STM_HR_REGION_START);
}

static inline uint16_t stm_protocol_u32_hi(uint32_t value)
{
    return (uint16_t)(value >> 16);
}

static inline uint16_t stm_protocol_u32_lo(uint32_t value)
{
    return (uint16_t)(value & UINT32_C(0xffff));
}

static inline uint32_t stm_protocol_u32_from_regs(uint16_t high, uint16_t low)
{
    return ((uint32_t)high << 16) | (uint32_t)low;
}

static inline void stm_protocol_u64_to_regs(uint64_t value, uint16_t out_regs[4])
{
    out_regs[0] = (uint16_t)(value >> 48);
    out_regs[1] = (uint16_t)(value >> 32);
    out_regs[2] = (uint16_t)(value >> 16);
    out_regs[3] = (uint16_t)value;
}

static inline uint64_t stm_protocol_u64_from_regs(const uint16_t regs[4])
{
    return ((uint64_t)regs[0] << 48) |
           ((uint64_t)regs[1] << 32) |
           ((uint64_t)regs[2] << 16) |
           (uint64_t)regs[3];
}

static inline uint16_t stm_protocol_pack_actuator_status(uint8_t stage1,
                                                         uint8_t stage2)
{
    return (uint16_t)(((uint16_t)stage2 << 8) | (uint16_t)stage1);
}

static inline uint8_t stm_protocol_stage1_from_status(uint16_t status)
{
    return (uint8_t)(status & UINT16_C(0x00ff));
}

static inline uint8_t stm_protocol_stage2_from_status(uint16_t status)
{
    return (uint8_t)(status >> 8);
}

static inline uint16_t stm_protocol_pack_origin_and_flags(uint8_t origin,
                                                          uint8_t flags)
{
    return (uint16_t)(((uint16_t)flags << 8) | (uint16_t)origin);
}

static inline uint8_t stm_protocol_origin_from_word(uint16_t word)
{
    return (uint8_t)(word & UINT16_C(0x00ff));
}

static inline uint8_t stm_protocol_flags_from_word(uint16_t word)
{
    return (uint8_t)(word >> 8);
}

static inline uint16_t stm_protocol_pack_event_state_and_severity(
    uint8_t event_state, uint8_t event_severity)
{
    return (uint16_t)(((uint16_t)event_severity << 8) |
                      (uint16_t)event_state);
}

static inline uint8_t stm_protocol_event_state_from_word(uint16_t word)
{
    return (uint8_t)(word & UINT16_C(0x00ff));
}

static inline uint8_t stm_protocol_event_severity_from_word(uint16_t word)
{
    return (uint8_t)(word >> 8);
}

static inline uint16_t stm_protocol_pack_version(uint8_t major, uint8_t minor)
{
    return (uint16_t)(((uint16_t)major << 8) | (uint16_t)minor);
}

static inline uint8_t stm_protocol_version_major(uint16_t version)
{
    return (uint8_t)(version >> 8);
}

static inline uint8_t stm_protocol_version_minor(uint16_t version)
{
    return (uint8_t)(version & UINT16_C(0x00ff));
}

#endif /* STM_PROTOCOL_H */
