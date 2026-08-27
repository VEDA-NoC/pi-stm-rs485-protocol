# VEDA4 NoC VMS 인터페이스 및 프로토콜 명세

이 저장소는 VEDA 4기 팀 NoC VMS 프로젝트에서 사용하는 외부 인터페이스와 공용 wire contract를 관리한다. 인터페이스 명세, RPI–STM 공용 header, 프레임·레지스터 검사 코드와 개발용 시험 도구를 제공한다.

## 시스템 연결 구조

```text
Camera ── RTSP/Camera HTTP API ──> RPI VMS
                                      │
                                      ├── RTSPS/HTTPS ──> Qt Client
                                      │
                                      └── RS-485 Modbus RTU ──> STM32
```

- Camera ↔ RPI VMS: 영상 입력, 주차 ROI와 주차 상태, 상태 변경 알림을 전달한다.
- RPI VMS ↔ Qt Client: 실시간·재생 영상과 HTTPS Control API를 제공한다.
- RPI VMS ↔ STM32: 센서·액추에이터 상태, event와 command를 Modbus RTU로 교환한다.

## 상세 명세

| 인터페이스 | 문서 | 범위 |
| --- | --- | --- |
| Camera–RPI | [camera-rpi/camera-rpi-interface.md](camera-rpi/camera-rpi-interface.md) | RTSP, Camera HTTP API, ROI·주차 상태·event |
| RPI–Qt | [rpi-qt/rpi-qt-interface.md](rpi-qt/rpi-qt-interface.md) | RTSPS, HTTPS Control API, 인증·TLS |
| RPI–STM | [rpi-stm/rpi-stm-interface.md](rpi-stm/rpi-stm-interface.md) | RS-485, Modbus RTU, register·command·event |

## 저장소 구성요소

- `camera-rpi/`: Camera와 RPI VMS 사이의 외부 계약을 관리한다.
- `rpi-qt/`: RPI VMS와 Qt Client 사이의 외부 계약을 관리한다.
- `rpi-stm/`: RPI VMS와 STM 사이의 외부 계약을 관리한다.
- `rpi-stm/stm_protocol.h`: RPI–STM 주소, bit, opcode, 단위와 codec의 최종 기준으로서 양쪽 코드에 포함하여 사용한다.
- `rpi-stm/header_compile_test.c`: 공용 header의 C11/C++17 호환성과 packing helper를 검사한다.
- `rpi-stm/pi-tester/`: RPI에서 Modbus RTU frame과 STM register를 검증하는 개발용 도구이다. 단위 테스트와 실제 STM 연동 시험을 제공한다.
- `rpi-stm/rpi-stm-design-guide.md`: RPI–STM 통신 방식 선정 근거, timing 계산과 운용 흐름을 설명하는 비규범 가이드이다.

## 관련 구현 저장소

| 구성요소 | 저장소 또는 런타임 경로 | 검토 기준 commit |
| --- | --- | --- |
| 인터페이스·protocol contract | [pi-stm-rs485-protocol](https://github.com/VEDA-NoC/pi-stm-rs485-protocol) | `16ca1444a721516fe37ba71b525c96d471d5530e` |
| RPI VMS | [RPI-Server](https://github.com/VEDA-NoC/RPI-Server), `src/stm` | `9b6c4e6fd56daf0b1887d4e178cceedf7dd54157` |
| Qt Client | [Qt-Client](https://github.com/VEDA-NoC/Qt-Client) | `4c0dfa91b45dbb4cb4b62c1bed4bc9be950de697` |
| Camera/OpenSDK | [edge-lpr](https://github.com/VEDA-NoC/edge-lpr) | `9aa7dcbafd2608ebbb497a8e66768d4a35bbb0ac` |
| STM firmware | [stm32-ev-firmware](https://github.com/VEDA-NoC/stm32-ev-firmware) | `286bcf3d98d488daede1f24ab62017c128014519` |

`pi-stm-rs485-protocol`의 commit만 이번 문서 재구성 전 기준점이며, 나머지 commit은 인터페이스 호환성을 검토한 구현 기준점이다.

## 명세 버전

- 통합 명세: `v0.0.2`
- RPI–STM wire protocol: `v0.2`
- 기준일: 2026-08-27
