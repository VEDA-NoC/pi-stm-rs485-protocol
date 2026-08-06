# P2.1 공용 header 데이터 명세 초안

## 이 문서가 하는 역할

이 문서는 `stm_protocol.h` v0.2의 사람이 읽는 데이터 명세다.

- 이 문서: 각 주소의 변수명, wire type, 단위, 값과 의미를 설명한다.
- `stm_protocol.h`: Pi와 STM 코드가 공유할 주소, bit, enum과 codec을 정의한다.
- 팀 가이드: 전체 통신 흐름과 설계 이유를 설명한다.
- 상세 설계: 대안, 계산과 이후 확장 근거를 보존한다.

주소와 숫자가 다르면 이 문서가 아니라 `stm_protocol.h`를 먼저 수정하고 이 표를
같이 갱신한다. 항목 1~6의 검토 결과와 남은 hardware 실측은 이 문서의
`v0.1 항목 1~6 검토 결과`를 단일 원본으로 사용한다.

v0.1의 wire contract 항목 1~6은 검토를 마쳐 아래 값으로 고정했다. v0.2에서는 기존
문서에 없던 Pi NTP 기준 시각 동기화를 `SYNC_TIME` command로 추가했다. `30ms` response
timeout과 `700ms` scheduler budget만 실제 STM 실측 뒤 확정한다.

## 공통 wire 규칙

| 항목 | 값 |
| --- | --- |
| 물리 계층 | RS485 2-wire half-duplex |
| master | Raspberry Pi |
| frame | Modbus RTU |
| UART wire | `19200 8E1` |
| STM32F401 UART | 9-bit word length, even parity, 1 stop |
| register 크기 | 16-bit unsigned |
| register 내부 byte 순서 | high byte first |
| 여러 register의 정수 순서 | high register first |
| 온도 | `uint16_t`, 0.01°C |
| 거리 | `uint16_t`, 1mm |
| STM local time | `uint64_t`, 1ms |

C의 `enum` 객체 크기는 compiler마다 달라질 수 있다. 이 명세에서 enum과 bitmap의
wire type은 모두 `uint16_t`이며 `sizeof(enum)`이나 packed structure를 전송하지
않는다. RTU의 slave address, function code, byte count와 CRC는 Modbus library가
직렬화한다.

## FC4 input-register map

| Window | 절대 주소 | Register 수 | 읽는 시점 |
| --- | ---: | ---: | --- |
| Base state | `0x0000` | 6 | STM별 최소 500ms 간격 |
| Command result | `0x0010` | 6 | 명령 접수 후 상태 확인 |
| Event status | `0x0020` | 21 사용, 32 예약 | pending/overflow일 때 |
| Identity | `0x0040` | 16 | 연결·재연결·점검 때 |
| Sensor detail | `0x0060` | 8 | 진단·상세 화면 요청 때 |
| Diagnostic reserved | `0x0080` | 32 예약 | v0.1 미사용 |

STM library에는 다음 두 개의 별도 연속 배열을 연결한다.

| Region | PDU 주소 | STM 배열 |
| --- | --- | --- |
| FC4 input | `0x0000..0x009f` | `input_regs[0..159]` |
| FC16 holding | `0x0100..0x015f` | `holding_regs[0..95]` |

PDU 주소가 실제 wire logical address다. `30001/40001` 형식은 사람과 일부 도구가
표시하는 reference일 뿐 frame에 넣지 않는다. 예를 들어 FC16 PDU `0x0100`은
reference `40257`이고 STM에서는 `holding_regs[0]`이다.

### Base state: `0x0000`, 6 registers

| 주소 | 변수명 | Wire type | 단위·값 | 의미 |
| --- | --- | --- | --- | --- |
| `0x0000` | `temperature_centi_c` | `uint16_t` | 0.01°C | 최신 적외선 온도. `4512`는 45.12°C |
| `0x0001` | `state_flags` | `uint16_t` bitmap | 아래 bit 표 | 자주 필요한 유효성·점유·화재·event·fault 요약 |
| `0x0002` | `temperature_age_ms` | `uint16_t` | ms | 측정 후 경과시간. `0..65534`, `0xffff`는 unknown/stale |
| `0x0003` | `actuator_status` | `uint16_t` | low byte=stage 1, high byte=stage 2 | 두 actuator의 현재 상태 |
| `0x0004` | `oldest_pending_event_sequence_hi` | `uint16_t` | sequence bits 31..16 | 가장 오래된 미확인 event 번호의 상위 word |
| `0x0005` | `oldest_pending_event_sequence_lo` | `uint16_t` | sequence bits 15..0 | 하위 word. queue가 비면 hi/lo 모두 0 |

`state_flags`:

| Bit | 이름 | 1의 의미 |
| ---: | --- | --- |
| 0 | `TEMPERATURE_VALID` | `temperature_centi_c`를 사용할 수 있음 |
| 1 | `TEMPERATURE_STALE` | 온도 갱신이 허용 지연을 넘음 |
| 2 | `OCCUPANCY_VALID` | 점유 판단을 신뢰할 수 있음 |
| 3 | `OCCUPIED` | 차량이 있다고 판단함 |
| 4 | `FIRE_ACTIVE` | STM local 화재 판정이 활성 상태임 |
| 5 | `EVENT_PENDING` | ACK되지 않은 event가 있음 |
| 6 | `EVENT_OVERFLOW` | event 유실이 발생했고 아직 Pi가 확인하지 않음 |
| 7 | `SENSOR_FAULT` | 하나 이상의 sensor fault가 있음 |
| 8 | `ACTUATOR_FAULT` | 하나 이상의 actuator fault가 있음 |
| 9 | `DEVICE_RESTARTED` | STM reboot를 Pi가 아직 확인하지 않음 |
| 10 | `REARM_REQUIRED` | 다음 진압 동작 전 수동 rearm이 필요함 |
| 11..15 | reserved | 송신 0 |

`actuator_status`의 stage별 8-bit 값:

| 값 | 이름 | 의미 |
| ---: | --- | --- |
| 0 | `NOT_PRESENT` | 해당 stage hardware 없음 |
| 1 | `IDLE` | 대기, 새 명령 가능 |
| 2 | `RUNNING` | STM local state machine이 동작 중 |
| 3 | `COMPLETE` | 동작 완료 |
| 4 | `FAILED` | 동작 실패 |
| 5 | `STOPPED` | 정지 명령 또는 local policy로 정지 |
| 6 | `INTERLOCKED` | 안전 interlock 때문에 실행 불가 |
| 7 | `REARM_REQUIRED` | 수동 rearm 전 재실행 금지 |

예:

```text
temperature_centi_c = 4512 -> 45.12°C
oldest_pending_event_sequence = 0x12345678
0x0004 = 0x1234, 0x0005 = 0x5678
```

### Command result: `0x0010`, 6 registers

| 주소 | 변수명 | Wire type | 값 | 의미 |
| --- | --- | --- | --- | --- |
| `0x0010` | `result_command_id_hi` | `uint16_t` | ID bits 31..16 | 결과가 가리키는 명령 ID |
| `0x0011` | `result_command_id_lo` | `uint16_t` | ID bits 15..0 | 명령 ID 하위 word |
| `0x0012` | `command_status` | `uint16_t` enum | 아래 표 | 명령 lifecycle |
| `0x0013` | `command_reason` | `uint16_t` enum | 아래 표 | 실패·거부 또는 특이 상태의 이유 |
| `0x0014` | `result_actuator_status` | `uint16_t` | base와 동일한 byte 배치 | 결과 시점의 stage 1/2 상태 |
| `0x0015` | `result_reserved` | `uint16_t` | 0 | 확장용 |

`command_status`:

| 값 | 이름 | 의미 |
| ---: | --- | --- |
| 0 | `NONE` | 아직 보고할 명령 없음 |
| 1 | `ACCEPTED` | 명령 검증과 queue 접수 완료 |
| 2 | `RUNNING` | 실행 중 |
| 3 | `COMPLETED` | 정상 완료 |
| 4 | `FAILED` | 실행을 시작했으나 실패 |
| 5 | `REJECTED` | 실행 전 거부 |

`command_reason`:

| 값 | 이름 | 의미 |
| ---: | --- | --- |
| 0 | `NONE` | 별도 이유 없음 |
| 1 | `UNSUPPORTED` | 지원하지 않는 opcode 또는 capability |
| 2 | `INVALID_ARGUMENT` | argument 형식·범위 오류 |
| 3 | `BUSY` | 현재 다른 동작 때문에 접수 불가 |
| 4 | `INTERLOCK` | STM local 안전 조건 때문에 거부·중단 |
| 5 | `FIRE_STILL_ACTIVE` | 화재 상태가 남아 rearm 불가 |
| 6 | `REARM_REQUIRED` | 수동 rearm 필요 |
| 7 | `LOCAL_FAULT` | sensor·actuator 등 STM local fault |
| 8 | `ID_CONFLICT` | 같은 command ID에 이전과 다른 opcode/argument가 들어옴 |

timeout retry는 같은 ID와 같은 payload를 보낸다. STM은 중복 실행하지 않고 그 ID의
기존 lifecycle 결과를 그대로 반환한다. 같은 ID에 다른 payload가 오면
`REJECTED/ID_CONFLICT`로 처리한다.

### Event status: `0x0020`, 21 registers

| 주소 | 변수명 | Wire type | 의미 |
| --- | --- | --- | --- |
| `0x0020` | `pending_event_count` | `uint16_t` | ACK를 기다리는 event 수 |
| `0x0021` | `event_status_flags` | `uint16_t` bitmap | bit0 overflow latched, bit1 critical overflow, bit2 normal overflow |
| `0x0022..23` | `dropped_event_count` | `uint32_t` | queue 부족으로 보존하지 못한 누적 event 수 |
| `0x0024..25` | `oldest_available_event_sequence` | `uint32_t` | 현재 STM에 실제 남은 가장 오래된 event 번호 |
| `0x0026..27` | `event_sequence` | `uint32_t` | 현재 event slot의 식별·ACK 번호 |
| `0x0028..29` | `event_boot_session_id` | `uint32_t` | event가 속한 STM boot session |
| `0x002a..2d` | `event_local_tick_ms` | `uint64_t` | 부팅 후 event 발생 monotonic tick, ms |
| `0x002e` | `event_type` | `uint16_t` enum | fire, occupancy, actuator, fault, restart, overflow 종류 |
| `0x002f` | `event_state_and_severity` | `uint16_t` | low byte=state, high byte=severity |
| `0x0030` | `event_payload_word_count` | `uint16_t` | 유효 payload register 수, `0..4` |
| `0x0031..34` | `event_payload[0..3]` | `uint16_t[4]` | event 종류별 payload, 미사용 word는 0 |

`event_type` 값은 `NONE=0`, `FIRE_STARTED=1`, `FIRE_CLEARED=2`,
`OCCUPANCY_CHANGED=3`, `STAGE1_STATE_CHANGED=4`,
`STAGE2_STATE_CHANGED=5`, `SENSOR_FAULT=6`, `ACTUATOR_FAULT=7`,
`DEVICE_RESTARTED=8`, `QUEUE_OVERFLOW=9`다.

`event_state`는 `OCCURRED=0`, `STARTED=1`, `ENDED=2`다.
`event_severity`는 `INFO=0`, `WARNING=1`, `CRITICAL=2`다.

v0.1 event payload 후보:

| Event | `payload0` | `payload1` | Word count |
| --- | --- | --- | ---: |
| `FIRE_STARTED/CLEARED` | `temperature_centi_c` | `state_flags` snapshot | 2 |
| `OCCUPANCY_CHANGED` | `occupied` 0/1 | `distance_mm` | 2 |
| `STAGE1/2_STATE_CHANGED` | `actuator_status` | `command_reason` 또는 0 | 2 |
| `SENSOR_FAULT` | `sensor_status_flags` | 구현별 diagnostic code | 2 |
| `ACTUATOR_FAULT` | `actuator_status` | 구현별 diagnostic code | 2 |
| `DEVICE_RESTARTED` | `reset_reason` | 미사용 0 | 1 |
| `QUEUE_OVERFLOW` | `dropped_event_count_hi` | `dropped_event_count_lo` | 2 |

이 payload schema는 v0.1로 승인한다. 현재 가장 큰 payload가 2 registers이므로
4 registers, 8-byte 한도 안에 4 bytes의 확장 여유가 있다.

### Identity: `0x0040`, 16 registers

| 주소 | 변수명 | Wire type | 의미 |
| --- | --- | --- | --- |
| `0x0040` | `protocol_version` | `uint16_t` | high byte=major, low byte=minor |
| `0x0041` | `firmware_version` | `uint16_t` | high byte=major, low byte=minor |
| `0x0042..43` | `capability_bitmap` | `uint32_t` | 설치된 hardware 기능 |
| `0x0044..45` | `device_uid_word0` | `uint32_t` | STM UID word0, high 16-bit 먼저 |
| `0x0046..47` | `device_uid_word1` | `uint32_t` | STM UID word1, high 16-bit 먼저 |
| `0x0048..49` | `device_uid_word2` | `uint32_t` | STM UID word2, high 16-bit 먼저 |
| `0x004a..4b` | `boot_session_id` | `uint32_t` | Pi가 STM reboot마다 할당하는 session ID |
| `0x004c` | `reset_reason` | `uint16_t` bitmap | power-on, pin, software, watchdog, brownout 등 |
| `0x004d` | `hardware_revision` | `uint16_t` | high byte=major, low byte=minor |
| `0x004e..4f` | `identity_reserved[0..1]` | `uint16_t[2]` | 0 |

`capability_bitmap`:

| Bit | 기능 |
| ---: | --- |
| 0 | infrared temperature sensor |
| 1 | ultrasonic sensor |
| 2 | LED |
| 3 | buzzer |
| 4 | character display |
| 5 | stage 1 servo |
| 6 | stage 2 pump |
| 7..31 | reserved 0 |

`reset_reason`은 여러 원인이 같이 표시될 수 있는 bitmap이다. bit0 power-on, bit1
external pin, bit2 software, bit3 watchdog, bit4 brownout, bit5 low-power, bit15
unknown을 사용한다.

### Sensor detail: `0x0060`, 8 registers

이 window는 base cycle마다 읽지 않는다.

| 주소 | 변수명 | Wire type | 단위·값 | 의미 |
| --- | --- | --- | --- | --- |
| `0x0060` | `distance_mm` | `uint16_t` | 1mm | 최신 초음파 거리 |
| `0x0061` | `distance_age_ms` | `uint16_t` | ms | 거리 측정 후 경과시간. `0xffff`는 unknown/stale |
| `0x0062` | `sensor_status_flags` | `uint16_t` bitmap | 아래 bit | 거리와 sensor 상세 상태 |
| `0x0063..66` | `occupancy_changed_tick_ms` | `uint64_t` | ms | 현재 STM boot에서 점유 판단이 마지막으로 바뀐 monotonic tick |
| `0x0067` | `sensor_detail_reserved` | `uint16_t` | 0 | 확장용 |

`sensor_status_flags`는 bit0 distance valid, bit1 distance stale, bit2 out of range,
bit3 ultrasonic fault, bit4 infrared fault, bit5 distance noisy, bit6 occupancy tick
valid를 사용한다. `distance_mm` 자체에 오류 sentinel을 넣지 않고 이 flag로 값의
유효성을 구분한다.

## FC16 command mailbox: `0x0100`, 8 registers

| 주소 | 변수명 | Wire type | 의미 |
| --- | --- | --- | --- |
| `0x0100` | `command_opcode` | `uint16_t` enum | 수행할 동작 종류 |
| `0x0101..02` | `command_id` | `uint32_t` | 실행 건 식별과 retry 중복 실행 방지 |
| `0x0103` | `command_origin_and_flags` | `uint16_t` | low byte=origin, high byte=flags |
| `0x0104` | `command_argument0` | `uint16_t` | opcode별 첫 번째 인자 |
| `0x0105` | `command_argument1` | `uint16_t` | opcode별 두 번째 인자 |
| `0x0106..07` | `command_reserved[0..1]` | `uint16_t[2]` | v0.2 송신자는 0 |

`command_opcode`:

| 값 | 이름 | argument |
| ---: | --- | --- |
| `0x0000` | `NONE` | 모두 0 |
| `0x0001` | `START_STAGE1` | 모두 0 |
| `0x0002` | `START_STAGE2` | 모두 0 |
| `0x0003` | `STOP_ACTUATOR` | argument0 bit0=stage1, bit1=stage2 |
| `0x0004` | `REARM` | 모두 0 |
| `0x0010` | `ACK_EVENT` | argument0/1=event sequence hi/lo |
| `0x0011` | `ACK_EVENT_STATUS` | argument0/1=확인한 dropped count hi/lo |
| `0x0012` | `ACK_RESTART` | argument0/1=새 boot session ID hi/lo |
| `0x0013` | `SYNC_TIME` | argument0/1=Pi Unix epoch seconds hi/lo |
| `0x0020` | `WRITE_CONFIG_COMMIT` | config window를 구현할 때 정의 |

`command_origin`은 `UNKNOWN=0`, `QT_ADMIN=1`, `PI_AUTOMATION=2`다. v0.2 flags는
항상 0이다.

### NTP 기준 시각 동기화: `SYNC_TIME`

`ACK_RESTART`는 STM 재부팅을 확인하고 event를 구분하는 `boot_session_id`용이다. Unix
epoch를 넣지 않는다. STM의 `uint64_t local_tick_ms`를 UTC 시간축에 연결할 때는 Pi가
`SYNC_TIME`을 unicast로 보낸다.

```text
command_opcode = SYNC_TIME (0x0013)
argument0/1   = Unix epoch seconds uint32 high/low
```

Pi는 자신의 system clock이 NTP로 동기화됐을 때만 이 command를 보낸다. STM은 FC16
command 처리 시점에 다음 두 값을 RAM에 함께 저장한다.

```text
epoch_seconds_at_sync
local_tick_ms_at_sync
```

그 boot session에서 이후 발생한 event의 UTC 추정값은 다음과 같다.

```text
utc_ms = epoch_seconds_at_sync * 1000
       + (event_local_tick_ms - local_tick_ms_at_sync)
```

STM은 NTP client나 RTC가 아니므로 power loss/reboot 뒤에는 이 기준이 사라진다. Pi는
`DEVICE_RESTARTED`를 확인하고 `ACK_RESTART`로 새 session을 부여한 뒤 `SYNC_TIME`을
보낸다. sync 이전 event는 absolute UTC로 해석하지 않고 Pi 수신시각과 local tick을
함께 보관한다.

wire 값은 **초** 단위이므로 버려진 sub-second 부분과 RS485 전달 지연만큼 UTC 추정
오차가 있다. 이 값은 event 시간축을 Pi/NTP 기준에 연결하는 용도이며, millisecond급
지연 측정의 기준은 계속 Pi에서 수집한 timestamp를 사용한다.

예:

```text
command_opcode = START_STAGE2
command_id = 1001

0x0100 = 0x0002
0x0101 = 0x0000
0x0102 = 0x03e9
0x0103 = 0x0001  # QT_ADMIN, flags=0
0x0104..0107 = 0
```

timeout 재전송은 같은 `command_id=1001`을 사용한다. 나중에 새로 stage 2를
실행하면 `command_id=1002`처럼 새 ID를 사용한다.

`0x0120..013f`는 configuration, `0x0140..015f`는 commissioning 용도로 예약하며
v0.1에서 미정인 register는 읽을 때 0, 쓸 때 0만 허용한다.

## Polling 규칙

Pi는 장치별 `last_base_poll_started_at`을 저장하고 같은 STM의 base state를
500ms보다 자주 읽지 않는다.

```text
next_sweep_start =
  max(previous_sweep_start + 500ms, previous_sweep_finish)
```

- STM 1대: 약 22ms transaction 뒤 약 478ms 대기
- STM 5대: 약 112ms sweep 뒤 약 388ms 대기
- STM 20대: 약 447ms sweep 뒤 약 53ms 대기
- sweep이 500ms를 넘으면 겹치지 않고 종료 직후 다음 sweep 시작
- CRC 오류·timeout은 전 장치 sweep 중 즉시 재시도하지 않음
- 전 장치 sweep 뒤 700ms budget이 남을 때만 실패 node 재시도
- command는 현재 RTU transaction 직후 우선 전송
- event read는 node당 sweep당 최대 1건

## STM과 Pi 적용 방식

STM은 input/holding register 배열을 준비하고 이 header의 절대 시작 주소와 상대
offset을 이용해 값을 갱신한다. FC16 frame 전체를 CRC까지 처리한 뒤 mailbox
8 registers를 한 번 snapshot하여 control queue로 넘긴다.

Pi는 register 값을 byte codec으로 조합한다. `stm_protocol_u32_*`,
`stm_protocol_u64_*`, actuator·origin·event packing helper를 양쪽에서 같이 사용한다.

## v0.1 항목 1~6 검토 결과

| 번호 | 결정 | 검토 결과 |
| ---: | --- | --- |
| 1 | `state_flags` bit 0..10 | 승인. validity → 상태 → event → fault/restart를 빈 bit 없이 연속 배치 |
| 2 | `actuator_status` 값 0..7 | 승인. 부재, 대기, 실행, 완료, 실패, 정지, interlock, rearm 필요를 구분 |
| 3 | opcode/status/reason | 승인. 중복 ID는 기존 결과 반환, 같은 ID의 다른 payload는 `ID_CONFLICT` |
| 4 | event payload | 승인. v0.1 최대 사용 2 registers, window는 4 registers/8 bytes 예약 |
| 5 | 96-bit UID 순서 | 승인. STM UID word0 → word1 → word2, 각 32-bit는 high → low register |
| 6 | FC4/FC16 주소 | 승인. input start `0x0000`, 160 words; holding start `0x0100`, 96 words |

남은 것은 실제 STM에서 response timeout `30ms`와 scheduler budget `700ms`를
측정하는 일뿐이다. 측정 전에도 개발 시험은 이 값을 기본값으로 사용하며 CLI option으로
timeout을 바꿔 비교할 수 있다.

## Raspberry Pi 개발 시험기

`pi-tester/`에는 이 명세를 사용하는 대화형 C++ Modbus RTU master가 있다.
build, dry-run, 실제 serial 시험과 STM register array 설정은
`pi-tester/README-ko.md`를 따른다.

## 초안 자체 검사

`header_compile_test.c`는 다음을 검사한다.

- C11과 C++17 `-Wall -Wextra -Werror -pedantic` compile
- FC4/FC16 window가 서로 겹치지 않는지
- reserved bit가 정의된 bit와 겹치지 않는지
- `uint32_t`, `uint64_t` register 분할과 재조합
- actuator, command origin/flags와 event state/severity packing
