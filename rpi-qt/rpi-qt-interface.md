# RPI–Qt 인터페이스 명세

## 1. 목적과 책임 경계

이 문서는 RPI VMS와 Qt Client 사이의 외부 계약을 정의한다.

| 구성요소 | 책임 |
| --- | --- |
| RPI VMS | RTSPS stream, HTTPS Control API, 인증·session, 녹화·주차·STM 상태 제공 |
| Qt Client | 서버 인증서 검증, 사용자 로그인, Bearer session 관리, API와 stream 소비 |

## 2. transport와 endpoint

| 용도 | scheme | 기본 port | path |
| --- | --- | ---: | --- |
| 실시간 영상 | RTSPS | `8554` | `/ch1` .. `/ch4` |
| 재생 영상 | RTSPS | `8554` | `/playback/{playback_session_id}` |
| Control API | HTTPS | `9443` | `/api/v1/...` |

Qt는 RTSP over TCP를 사용한다. 실시간 stream의 channel은 VMS channel `1..4`이며 Camera channel 번호를 직접 노출하지 않는다. 재생 path는 playback session 생성 응답의 `rtsps_path`를 최종값으로 사용한다.

## 3. TLS와 서버 인증

- HTTPS와 RTSPS는 TLS 1.2 이상을 사용한다.
- Qt는 지정한 `server.crt`를 요청별 TLS 구성의 CA certificate 목록에 추가하고 peer certificate를 검증한다.
- Qt는 검증에 성공한 peer의 leaf certificate DER에 대한 SHA-256 digest를 pin 값과 비교한다. (이는 SPKI pin이 아니다.)
- 인증서 누락, chain/hostname 검증 실패 또는 leaf pin 불일치를 연결 실패로 처리하며 TLS 오류를 무시하지 않는다.
- 계정정보와 token은 URL이나 일반 log에 기록하지 않는다.

## 4. HTTPS Control API 공통 계약

### 4.1 인증

로그인 요청은 HTTPS 위의 HTTP Basic 인증을 사용한다. Qt는 `username:password`를
Base64로 인코딩하여 전송한다. 계정정보의 전송 기밀성은 TLS 1.2 이상, 인증서 검증과 leaf certificate pinning에서 이루어진다. 
RPI는 password 원문을 저장하지 않고 PBKDF2-HMAC-SHA256 verifier로 검증한다.

```http
POST /api/v1/login
Authorization: Basic base64(username:password)
```

성공 응답의 주요 필드는 다음과 같다.

| 필드 | 형식·단위 | 의미 |
| --- | --- | --- |
| `schema_version` | 정수 | JSON schema version 반환 |
| `token_type` | 문자열 `Bearer` | 보호 API 인증 방식 반환 |
| `access_token` | 불투명 문자열 | session token 반환 |
| `expires_in_seconds` | 정수, second | token 유효기간 반환 |

로그인 외 모든 API는 `Authorization: Bearer {access_token}`을 사용한다. `DELETE /api/v1/session`은 현재 token을 폐기한다. 보호 API에서 `401 Unauthorized`를 받으면 Qt는 local session과 token을 폐기하고 재로그인을 요구한다.

### 4.2 요청·응답 상관관계

Client는 유효한 불투명 ASCII 값의 `X-Request-ID`를 선택적으로 전송할 수 있다. RPI는 유효한 값을 사용하고, 없거나 유효하지 않으면 새 ID를 생성한다. 모든 HTTP 응답은 `X-Request-ID`를 반환한다.

### 4.3 JSON과 시각·단위

| 표기 | 의미 |
| --- | --- |
| `*_utc_ms` | UTC Unix epoch millisecond |
| `monotonic_ms` | RPI boot 내 monotonic millisecond, UTC로 변환 금지 |
| `*_seconds` | second |
| `*_bytes` | byte |
| `temperature_celsius`, `temperature_c` | 섭씨 |
| `distance_mm` | millimeter |
| `channel_id` | VMS channel `1..4` |

JSON body가 있는 request는 `Content-Type: application/json`을 사용한다. 응답 schema의 현재 version은 `1`이다. 알 수 없는 추가 필드는 호환성을 위해 무시할 수 있지만 필수 필드의 type 오류는 응답 오류로 처리한다.

### 4.4 오류 객체와 HTTP status

일반 오류 body는 다음 형식이다.

```json
{"error":"machine_readable_code"}
```

일부 validation 응답은 `active_version`, `draft_version`, `validation_errors`를 추가한다.

| status | 의미 | Client 처리 |
| ---: | --- | --- |
| `200` | 조회·재사용·삭제 성공 | 응답 사용 |
| `201` | resource 생성 성공 | ID 저장 |
| `202` | 비동기 apply 접수 | job 조회 |
| `400` | query/body/header 값 오류 | 요청 수정 |
| `401` | 로그인 실패 또는 session 만료 | token 폐기, 재로그인 |
| `404` | resource 없음 | local 선택 갱신 |
| `409` | version/idempotency/resource 상태 충돌 | 최신 상태 조회 후 재시도 판단 |
| `415` | JSON media type 누락 | 요청 거부 |
| `422` | 주차 설정 validation 실패 | 오류 목록 표시, draft 수정 |
| `502` | Camera 연동 응답 실패 | apply/import 실패 표시 |
| `503` | service 또는 하위 장치 비활성 | 재시도 또는 운영 확인 |

## 5. 상태·timeline·thumbnail·재생

| 기능 | 요청 | 주요 응답 |
| --- | --- | --- |
| 상태 | `GET /api/v1/status` | `service`, `status`, `server_time`, `system`, `storage` 반환 |
| timeline | `GET /api/v1/channels/{channel_id}/timeline?start_utc_ms={s}&end_utc_ms={e}` | `channel_id`, 범위, `spans`, `events` 반환 |
| thumbnail | `GET /api/v1/channels/{channel_id}/thumbnail?utc_ms={t}&width={w}` | `image/jpeg` 반환 |
| 범위 재생 생성 | `POST /api/v1/playback-sessions?channel_id={id}&start_utc_ms={s}&end_utc_ms={e}` | playback session 반환 |
| event 재생 생성 | `POST /api/v1/playback-sessions?event_request_id={id}` | playback session 반환 |
| 재생 종료 | `DELETE /api/v1/playback-sessions/{id}` | `status=removed` 반환 |

`start_utc_ms`는 0 이상, `end_utc_ms`는 시작보다 커야 한다. thumbnail `width`는 현재 `160..1920`을 허용한다. playback 생성은 범위 query와 `event_request_id` 중 정확히 하나만 사용한다.

playback session 주요 필드는 다음과 같다.

| 필드 | 의미 |
| --- | --- |
| `playback_session_id` | 64자리 lowercase hexadecimal capability ID 반환 |
| `rtsps_path` | `/playback/{id}` 반환 |
| `channel_id` | VMS channel 반환 |
| `start_utc_ms`, `end_utc_ms`, `duration_ms` | 재생 UTC 범위와 길이 반환 |
| `segment_count` | 사용 segment 수 반환 |
| `one_shot` | 일회성 session 여부 반환 |

## 6. 주차구역 설정 API

### 6.1 resource와 endpoint

| 기능 | 요청 | 결과 |
| --- | --- | --- |
| active 조회 | `GET /api/v1/parking-spaces?channel_id={id}` | active configuration 반환 |
| ID 할당 | `POST /api/v1/parking-space-id-allocations` | 새 `space_id` 반환 |
| Camera import | `POST /api/v1/parking-space-imports` | Camera ROI를 draft로 반환 |
| draft 생성 | `POST /api/v1/parking-space-drafts` | draft 반환 |
| draft 수정 | `PUT /api/v1/parking-space-drafts/{draft_id}` | 증가한 `draft_version` 반환 |
| 검증 | `POST /api/v1/parking-space-drafts/{draft_id}/validate` | `valid`, 오류 목록 반환 |
| 적용 | `POST /api/v1/parking-space-drafts/{draft_id}/apply` | apply job 반환 |
| job 조회 | `GET /api/v1/parking-space-apply-jobs/{job_id}` | job 상태 반환 |

apply 요청은 `Idempotency-Key` header를 필수로 사용한다. 같은 key와 같은 draft는 기존 job을 반환할 수 있고, 다른 draft에 key를 재사용하면 `409`로 거부한다. `base_version`이 active version과 다르면 충돌로 거부한다. Camera read-back 검증까지 성공한 뒤에만 새 active version으로 승격한다.

### 6.2 주요 JSON field

| object | field | 의미 |
| --- | --- | --- |
| configuration | `draft_id`, `channel_id`, `base_version`, `active_version`, `draft_version`, `geometry_id` | version과 좌표계 식별자 사용 |
| space | `space_id`, `label`, `space_type`, `enabled`, `polygon` | 주차구역 정의 사용 |
| polygon point | `x`, `y` | 정규화 좌표 사용 |
| STM mapping | `device_uid`, `sensor_zone_id` | 선택적 STM 연결 사용 |
| validation error | `code`, `message`, `space_id` | 수정 대상 반환 |
| apply job | `job_id`, `draft_id`, `state`, `error_code`, `active_version`, `draft_version` | 적용 진행·결과 반환 |

## 7. event long polling

```http
GET /api/v1/events?after_id={last_event_id}&wait_ms={wait_ms}&limit={limit}
Authorization: Bearer ...
```

`wait_ms`는 long polling 대기시간, `limit`는 한 응답의 최대 event 수다. `after_id`는 마지막으로 처리한 event ID이며 응답의 `next_after_id`로 갱신한다.

| 응답 field | 의미 |
| --- | --- |
| `schema_version` | batch schema version 반환 |
| `server_utc_ms` | 응답 시점의 RPI UTC Unix epoch millisecond 반환 |
| `next_after_id` | 다음 요청 cursor 반환 |
| `events` | 순서 있는 event 배열 반환 |

event의 공통 field는 `event_id`, `event_type`, `phase`, `source_type`, `severity`, `timestamp_utc_ms`, `occurred_at_utc_ms`, `received_at_utc_ms`, `channel_id`, `camera_channel`, `correlation_id`, `schema_version`, `payload`다. `occurred_at_utc_ms`를 알 수 없으면 `null`을 반환하며 `received_at_utc_ms`와 혼동하지 않는다.

RPI는 자신이 관측한 Camera/STM event를 영속 저장한 뒤 cursor 기반으로 제공하므로 Qt의 polling 재연결은 `after_id`부터 이어갈 수 있다. 그러나 Camera 연결 단절 중 Camera에서 발생하고 종료되어 RPI가 전혀 관측하지 못한 전이는 이 API로 복구할 수 없다.

## 8. STM 장치와 명령 API

| 기능 | 요청 | 결과 |
| --- | --- | --- |
| 장치 조회 | `GET /api/v1/stm-devices` | `schema_version`, `devices` 반환 |
| 명령 제출 | `POST /api/v1/stm-commands` | `result`, `command_id` 반환 |
| 명령 조회 | `GET /api/v1/stm-commands?slave_address={a}&command_id={id}` | 명령 상태 반환 |
| 등록 해제 | `DELETE /api/v1/stm-registrations/{device_uid}` | 해제된 mapping 반환 |

명령 request body는 다음 field를 사용한다.

```json
{"slave_address":1,"opcode":1,"argument0":0,"argument1":0}
```

`argument0`, `argument1`은 선택적 unsigned 16-bit 값이다. 외부에 허용하는 opcode는 `START_STAGE1(0x0001)`, `START_STAGE2(0x0002)`, `STOP_ACTUATOR(0x0003)`, `REARM(0x0004)`이다. `ACK_*`, `SYNC_TIME` 등 RPI 내부 housekeeping opcode는 Qt에 노출하지 않는다. command `origin`은 Client가 보내지 않으며 RPI가 Qt Admin 요청으로 고정한다.

장치 응답의 주요 field는 `device_uid`, `slave_address`, `link`, `assigned_boot_session_id`, `registration`, `state`이다. `state`에는 온도와 validity/stale, 점유·화재·fault·rearm, stage별 status가 포함된다. command 조회는 `command_id`, `opcode`, `origin`, `status`, `reason`, `completed`를 반환한다. wire 값의 의미는 [RPI–STM 명세](../rpi-stm/rpi-stm-interface.md)를 따른다.

## 9. 연결 종료와 재시도

- RTSPS 종료 시 decoder와 session을 정리하고 사용자 동작 또는 Client 정책에 따라 재연결한다.
- HTTPS transport 오류는 HTTP application 오류와 구분한다.
- 인증서 오류는 자동 우회하지 않는다.
- `401`은 재시도 loop로 처리하지 않고 session 만료로 처리한다.
- 조회와 long polling은 동일 cursor/조건으로 재시도할 수 있다.
- 생성·명령·apply는 응답을 잃은 경우 resource ID, command status 또는 idempotency key로 기존 결과를 먼저 확인한다.
