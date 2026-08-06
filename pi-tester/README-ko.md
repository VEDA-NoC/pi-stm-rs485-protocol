# Raspberry Pi용 P2.1 STM Modbus RTU 개발 시험기

## 목적

이 프로그램은 제품용 Pi server가 아니라 STM 담당자와 protocol을 맞추기 위한 개발
도구다. Raspberry Pi가 Modbus RTU master가 되어 FC4 read와 FC16 command write를
실제로 수행한다.

시험기는 매 단계에서 다음을 출력한다.

1. 다음에 입력할 값과 의미
2. PDU logical address, 3xxxx/4xxxx reference와 STM 배열 index
3. 16-bit register별 16진수·10진수
4. field별로 묶은 실제 RTU frame과 연속 byte
5. 응답 시간, 응답 원시 byte, CRC·slave·FC·길이 검사
6. schema 해석 결과와 사용자가 입력한 기대 register 값의 PASS/FAIL
7. command write 후 같은 command ID의 진행·완료 결과
8. Pi의 현재 Unix epoch seconds를 기본값으로 제안하는 `SYNC_TIME` 시험

## 주소를 읽는 법

실제 wire에 들어가는 주소는 PDU logical address다.

| 구분 | 예 | 용도 |
| --- | --- | --- |
| FC4 PDU address | `0x0000` | 실제 request의 start address |
| 3xxxx reference | `30001` | 사람과 일부 Modbus 도구가 표시하는 번호 |
| STM input index | `0` | `input_regs[pdu_address - 0x0000]` |
| FC16 PDU address | `0x0100` | 실제 request의 start address |
| 4xxxx reference | `40257` | `40001 + 0x0100`인 설명용 번호 |
| STM holding index | `0` | `holding_regs[pdu_address - 0x0100]` |

`30001`이나 `40257`을 RTU frame에 넣으면 안 된다. 예를 들어 base-state request는
PDU address `0x0000`을 보내고, command mailbox는 `0x0100`을 보낸다.

## STM library 설정

권장 방식은
[`alejoseb/Modbus-STM32-HAL-FreeRTOS`](https://github.com/alejoseb/Modbus-STM32-HAL-FreeRTOS)의
separate memory region을 사용하는 것이다.

```c
#include "stm_protocol.h"

static uint16_t input_regs[STM_IR_REGION_COUNT];    /* 160 words */
static uint16_t holding_regs[STM_HR_REGION_COUNT];  /* 96 words */

ModbusH.u16regs = NULL;
ModbusH.u16regsize = 0;

ModbusH.u16inputRegs = input_regs;
ModbusH.u16inputRegsStartAdd = STM_IR_REGION_START;   /* 0x0000 */
ModbusH.u16inputRegsNregs = STM_IR_REGION_COUNT;

ModbusH.u16holdingRegs = holding_regs;
ModbusH.u16holdingRegsStartAdd = STM_HR_REGION_START; /* 0x0100 */
ModbusH.u16holdingRegsNregs = STM_HR_REGION_COUNT;
```

이 library는 PDU address에서 각 `StartAdd`를 빼 배열 index를 계산한다. 따라서
`0x0100` command write는 `holding_regs[0]`부터 기록된다. STM에 vendoring된 구버전이
separate region을 지원하지 않으면 shared `u16regs[0x0160]`도 동작하지만, FC4와
FC16 memory가 분리되지 않으므로 library update 또는 해당 기능 backport를 권장한다.

library의 FC16 handler는 `ModBusSphrHandle` semaphore를 잡은 상태에서 모든 word를
기록한다. STM control task도 같은 semaphore를 잡고 8-word mailbox를 local 변수로
한 번 복사한 뒤 release하고, 새 `command_id`일 때만 control queue에 넣는다. 그러면
절반만 갱신된 command를 실행하지 않는다.

확인한 upstream HEAD:

```text
b12d9894cd05994ed1eb8b4a559bd6cb44da069d
```

## UART와 RS485 준비

wire 설정은 `19200 8E1`이다.

- USB-RS485 adapter가 송수신 방향을 자동 전환하면 추가 option이 필요 없다.
- Pi UART와 direction-control 가능한 RS485 transceiver를 직접 사용하고 kernel
  driver가 `TIOCSRS485`를 지원하면 `--kernel-rs485`를 사용한다.
- A/B 극성, common ground 필요 여부, 양 끝 120Ω termination은 실제 module 사양에
  맞춘다.
- serial 장치가 `/dev/ttyUSB0`가 아닐 수 있으므로 `ls -l /dev/ttyUSB* /dev/ttyACM*`
  로 확인한다.

## Raspberry Pi에서 build

시작 경로는 이 `pi-tester` 폴더다.

```bash
make clean
make -j2
make test
```

생성 파일:

```text
bin/p2-stm-modbus-test
```

STM 개발자에게 ARM 실행파일과 source를 한 묶음으로 전달하려면 같은 Pi에서 실행한다.

```bash
make package
```

생성 파일:

```text
dist/p2-stm-modbus-tester-pi.tar.gz
```

이 archive에는 Pi에서 방금 build한 실행파일, source, 공용 header, unit test와 데이터
명세가 함께 들어간다.

CMake를 선호하면 다음도 지원한다.

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
ctest --test-dir build-cmake --output-on-failure
```

## 실제 송신 전 dry-run

```bash
./bin/p2-stm-modbus-test --dry-run --slave 3
```

메뉴에서 `R`을 선택하면 read window 표가 먼저 나온다. 예를 들어 base state의
기대값을 비교하려면 다음처럼 입력한다.

```text
선택> R
read 번호 1..5> 1
기대값> 0=4512,1=0x003d
```

`offset=값`이며 값은 10진수 또는 `0x` 16진수다. Enter만 누르면 기대값 비교를
생략하고 통신·CRC·schema만 검사한다.

stage 2 command frame만 만들려면 다음처럼 선택한다.

```text
선택> W
command 번호 1..8> 2
command_id> 1001
origin> 1
```

실제 송신 모드에서 `START_STAGE2`는 먼저 `STAGE2` 문자열을 다시 입력하고 최종
`y` 확인까지 해야 송신된다. dry-run에서는 이 안전 확인 없이 frame만 출력한다.

Pi NTP 기준 시각 sync frame은 다음처럼 만든다.

```text
선택> W
command 번호 1..8> 8
Pi Unix epoch seconds (Pi NTP 동기화 확인, 기본은 현재 system clock)>
```

시험기는 현재 Pi system clock의 Unix epoch seconds를 기본값으로 제안하고
`argument0/1`에 high/low 16-bit로 넣는다. `ACK_RESTART`의 argument는 여전히
`boot_session_id`이며, epoch 시간을 넣는 명령이 아니다.

retry한다면 `command_id`뿐 아니라 입력한 epoch seconds도 그대로 사용한다. 새 현재
시각을 넣으려면 새 command ID를 사용한다. 초 단위 sync이므로 sub-second 정밀도는
보장하지 않으며 Pi가 기록한 timestamp가 지연 측정의 기준이다.

출력 register는 다음 형태다.

```text
HEX: 0002 | 0000 | 03e9 | 0001 | 0000 | 0000 | 0000 | 0000
DEC: 2    | 0    | 1001 | 1    | 0    | 0    | 0    | 0
```

RTU frame은 다음처럼 field와 16-bit register 경계를 나눠 표시한다.

```text
[slave] | [FC] | [start 16-bit] | [count 16-bit] | [byte count]
| [HR0] | [HR1] | ... | [HR7] | [CRC low high]
```

## 실제 STM과 송수신

USB 자동 방향 제어 adapter 예:

```bash
./bin/p2-stm-modbus-test --device /dev/ttyUSB0 --slave 3
```

Pi UART kernel RS485 mode 예:

```bash
./bin/p2-stm-modbus-test --device /dev/ttyAMA0 --slave 3 --kernel-rs485
```

일반 사용자는 serial group 권한이 필요할 수 있다. 권한 오류가 나면 장치 group을
확인하고 사용자를 해당 group에 추가한 뒤 다시 로그인한다. 시험 때문에 무조건
`sudo`로 실행하지 않는다.

## Command 결과 확인

FC16 정상 응답은 “8 registers가 기록됐다”는 뜻이지 STM local command 처리 완료가
아니다.
프로그램은 write 성공 뒤 다음을 물어본다.

```text
Command result를 500ms 간격으로 확인합니까?
의도한 terminal status:
3=COMPLETED, 4=FAILED, 5=REJECTED
```

같은 `command_id`가 반환되는지 먼저 확인한 다음 terminal status를 비교한다.
`RUNNING`이면 500ms 뒤 다시 읽고, 의도한 값과 다르게 `FAILED/REJECTED`가 오면
reason과 actuator state를 함께 출력한다.

## 실패 판정

| 출력 | 우선 확인할 내용 |
| --- | --- |
| `response timeout` | slave 주소, A/B 극성, DE 방향, UART 8E1, STM T35 |
| `CRC16 불일치` | baud/parity, noise, frame 경계 |
| `Illegal Function` | STM library에서 FC4/FC16 활성 여부 |
| `Illegal Data Address` | `StartAdd`, 배열 크기와 PDU 주소 |
| slave/FC 불일치 | 다른 node 응답 또는 bus collision |
| command ID `WAIT` | STM이 command mailbox를 control task로 넘겼는지 |
| terminal status `FAIL` | `command_reason`, interlock와 local fault |
| `SYNC_TIME` 뒤 UTC가 틀림 | Pi NTP 상태, epoch high/low 순서, STM의 local tick capture |

## 파일

| 파일 | 역할 |
| --- | --- |
| `../stm_protocol.h` | Pi/STM 공용 wire contract |
| `src/modbus_rtu.*` | CRC, frame codec, parser와 Linux serial |
| `src/main.cpp` | 친절한 대화형 read/write 시험기 |
| `tests/test_modbus.cpp` | hardware 없는 protocol unit test |
| `Makefile` | Pi에서 binary와 unit test build |
| `CMakeLists.txt` | 선택 가능한 CMake build |
