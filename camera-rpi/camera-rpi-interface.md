# Camera–RPI 인터페이스 명세

## 1. 목적과 책임 경계

이 문서에서는 Camera가 외부에 제공하고 RPI VMS가 사용하는 영상·주차 인터페이스를 정의한다.

| 구성요소 | 책임 |
| --- | --- |
| Camera | RTSP 영상 제공, Digest 인증, 주차 ROI 저장·조회, 현재 주차 상태 제공, 상태 변경 알림 제공 |
| RPI VMS | Camera 연결 관리, channel 변환, ROI draft 생성·적용, 상태 snapshot 차이 계산, 이벤트·녹화 metadata 저장 |

주차 상태·ROI API와 `ParkingOccupied` event는 VEDA NoC 팀이 정의한 공개 interface이다.
한화비전 제품에 종속되는 SUNAPI 접속 명령, OpenSDK 내부 구현과 SDK 문서 내용은 이
공개 명세에 포함하지 않고 비공개 Camera 구현 저장소에서 관리한다. 실제 계정,
password, Camera 주소, 실제 차량번호와 원본 XML fixture도 공개하지 않는다.

## 2. 식별자와 channel 매핑

| 의미 | 형식 | 예 | 규칙 |
| --- | --- | --- | --- |
| Camera channel | 정수 `0..3` | `0` | Camera 영상·주차 API 기준 사용 |
| VMS channel | 정수 `1..4` | `1` | 이후 Qt Client에서 동일하게 사용 |
| Camera 주차구역 ID | 문자열 | `ch0-01` | Camera가 반환한 stable ID 사용 |
| VMS 주차구역 ID | 문자열 | `G-01` | RPI active configuration의 `space_id` 사용 |

RPI는 apply/read-back에서 얻은 mapping을 active configuration과 함께 저장한다.

## 3. RTSP 영상 입력

### 3.1 연결 계약

| 항목 | 값 |
| --- | --- |
| 전송 | RTSP over TCP 사용 |
| 인증 | Camera 계정의 Digest 인증 사용 |
| 영상 codec | H.264 또는 H.265 입력 처리 |
| 변환 범위 | depayload·parse 후 원본 codec 전달, transcoding 미사용 |
| RTSP path | 배포 설정에서 channel별 path 제공, 공개 명세에서 제품별 template 미정의 |

RPI는 Camera 계정정보를 RTSP URL 문자열에 삽입하지 않고 GStreamer `rtspsrc`의 인증 property로 전달한다. 연결이 끊기거나 RTSP pipeline에 오류가 발생하면 해당 channel을 비정상으로 표시하고 pipeline을 정리한 뒤 재연결 정책에 따라 다시 연결한다. 재연결 중에는 frame 연속성을 보장하지 않는다.

H.264/H.265는 Camera 입력 codec 선택 범위다. RPI는 Camera 내부 encoder 설정이나 codec profile 협상 규칙을 정의하지 않는다.

## 4. Camera HTTP API 인증과 공통 형식

주차 상태·ROI HTTP 요청은 Camera 계정의 Digest 인증을 사용한다. 계정정보는 runtime
secret으로 주입하며 저장소나 log에 평문으로 남기지 않는다.

| 항목 | 계약 |
| --- | --- |
| 요청 method | 각 endpoint 표의 `GET` 또는 `POST` 사용 |
| 응답 body | 팀 정의 주차 상태·ROI 응답의 XML 사용 |
| channel query | Camera channel `0..3` 사용 |
| 좌표 | 정규화 좌표 사용, 각 점의 `x`, `y`를 `0.0..1.0` 범위로 취급 |
| 오류 처리 | HTTP 실패, Digest 실패, XML parse 실패를 정상 응답과 구분 |

## 5. 주차 상태 조회

### 5.1 요청

```http
GET /opensdk/object_detect/parking_status
Authorization: Digest ...
```

### 5.2 응답 의미

응답 XML은 각 `space`의 다음 정보를 제공한다.

| XML 항목 | 의미 | RPI 처리 |
| --- | --- | --- |
| `space/@id` | Camera 주차구역 ID | mapping key로 사용 |
| `space/@channel` | Camera channel | worker channel과 일치 여부 확인 |
| `occupied` | 점유 여부 | 이전 snapshot과 비교 |
| `plate` | Camera가 인식한 차량번호 | event에는 masking 후 사용, 원문 미보존 |
| `ev/@source`, `ev` | 전기차 판정 근거와 값 | 주차 정책 입력으로 사용 |
| `violation` | 위반 여부 | event payload로 변환 |

RPI는 `parking_status`를 현재 상태 snapshot으로 사용한다. `ParkingOccupied` 알림 하나를 개별 구역의 확정 상태로 간주하지 않고, 알림 수신 시 전체 status를 다시 조회하여 이전 snapshot과 차이를 계산한다.

## 6. 주차 ROI 조회·등록·삭제

### 6.1 endpoint

| 작업 | 요청 | body | 운영 사용 |
| --- | --- | --- | --- |
| 조회 | `GET /opensdk/object_detect/parking_roi?ch={camera_channel}` | 없음 | import와 apply read-back에 사용 |
| 등록 | `POST /opensdk/object_detect/parking_roi?ch={camera_channel}&id={camera_space_id}` | `x1 y1 ... x4 y4` | 신규 등록 또는 삭제 후 재등록에 사용 |
| 원좌표 등록 | 위 등록 요청에 `&noflip=1` 추가 | `x1 y1 ... x4 y4` | Camera 좌표를 그대로 재전송할 때 사용 |
| 삭제 | `POST /opensdk/object_detect/parking_roi?ch={camera_channel}&delete={camera_space_id}` | 없음 | 제거에 사용 |
| channel 전체 삭제 | `POST /opensdk/object_detect/parking_roi?ch={camera_channel}&clear=1` | 없음 | 지원 operation, 정상 apply 절차에서는 미사용 |

등록 body는 공백으로 구분한 네 꼭짓점의 정규화 좌표 8개를 사용한다. `id`는 URL encoding한다. 조회 응답의 `channel`과 요청 channel이 다르면 metadata 불일치로 거부한다.

### 6.2 좌표 반전

현재 Camera ROI 조회 응답은 `flipped="true"`를 반환한다. RPI import는 이를 VMS 편집 좌표계로 변환하고 draft만 만든다. RPI가 VMS draft를 Camera에 적용할 때는 일반 등록 요청을 사용하여 Camera의 좌표 변환을 거친다. 적용 후 반드시 ROI를 다시 조회하여 ID, channel과 polygon을 검증한다.

정상 apply는 active configuration과 draft의 차이를 계산하여 구역별 등록·삭제를 수행한다. `clear=1`은 Camera API가 지원하지만, 영향 없는 ROI와 진행 중인 점유 session을 보존하기 위해 RPI 정상 운영 절차로 사용하지 않는다.

## 7. 주차 상태 변경 알림

### 7.1 연결

Camera는 장시간 유지되는 인증된 event stream으로 팀 정의 event
`ParkingOccupied`의 상태 변경을 알린다. 제품별 SUNAPI 구독 endpoint와 연결 명령은
비공개 Camera 구현 저장소에서 관리한다. RPI는 이 알림을 wake-up signal로만 사용하고,
매번 `parking_status`를 조회하여 실제 구역별 전이를 결정한다.

### 7.2 종료와 재연결

- 정상 EOF, socket 오류, HTTP 오류와 XML framing 오류를 연결 종료로 처리한다.
- 연결 종료 후 backoff를 적용하여 재연결한다.
- 마지막으로 관측한 주차 snapshot과 진행 중인 RPI parking session은 TCP 재연결만으로 삭제하지 않는다.
- 재연결 뒤 첫 유효 status snapshot을 직전 snapshot과 비교한다.

### 7.3 event gap

RPI는 관측하여 생성한 `started`, `updated`, `ended` event를 SQLite/녹화 인덱스에 저장한다. 연결 단절 중 점유가 시작되고 종료되어 재연결 시 상태가 다시 원래 값이면, Camera API에 sequence number나 history replay가 없으므로 RPI는 그 전이를 복구할 수 없다. 연결 전 시작을 관측하고 연결 후 종료 상태를 관측한 경우에는 기존 session과 상관관계를 이어간다.

따라서 다음을 구분한다.

| 상황 | 처리 |
| --- | --- |
| RPI가 관측한 event | 영속 저장 후 Qt에 제공 |
| 단절 전후 snapshot 차이 존재 | 차이를 event로 생성 |
| 단절 중 시작·종료 후 원상복귀 | 복구 불가, 현재 별도 gap event 영속 저장 미구현 |


## 8. 오류 처리

| 오류 | RPI 처리 |
| --- | --- |
| Digest 인증 실패 | 요청 실패 처리, secret 비노출, 설정 확인 필요 |
| HTTP `4xx/5xx` | operation 실패 반환, status와 endpoint 기록 |
| XML parse 실패 | 응답 거부, 원문 개인정보 비기록, 재조회 또는 재연결 |
| channel/ROI metadata 불일치 | draft/apply 거부 |
| apply 후 read-back 불일치 | apply job 실패 처리, active version 미승격 |
| RTSP/event stream 연결 종료 | channel 상태 갱신 후 backoff 재연결 |

재시도 가능한 조회와 연결 operation만 자동 재시도한다. ROI 변경은 중복 적용 영향과 read-back 결과를 확인하여 처리하며, 실패한 draft를 active configuration으로 승격하지 않는다.

## 9. 개인정보 처리 경계

- Camera가 반환한 원본 차량번호는 Camera–RPI 경계에서만 일시 처리한다.
- RPI event payload에는 masking한 차량번호만 저장·전달한다.
- 원본 `parking_status` XML과 실제 차량번호를 log, fixture에 남기지 않는다.
- 인증정보, Camera IP와 배포별 endpoint 설정은 runtime secret/configuration으로 관리한다.

## 10. 공개 범위와 호환성

| 구분 | 관리 범위 |
| --- | --- |
| 공개 interface | RTSP over TCP, H.264/H.265, Digest 인증, channel mapping, 팀 정의 주차 상태·ROI API와 `ParkingOccupied` event 의미 |
| 비공개 Camera 구현 | 제품별 RTSP path, SUNAPI event stream 구독 명령, OpenSDK 내부 구현과 SDK 문서 내용 |
| 호환성 확인 | Camera firmware·app 변경 시 XML schema, ROI 좌표 변환과 event framing 재검증 |
