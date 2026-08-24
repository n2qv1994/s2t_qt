# s2t_qt — Danh sách API cho client bên ngoài

Tài liệu này mô tả **toàn bộ giao diện lập trình của dịch vụ xử lý âm thanh**
mà `s2t_qt` sử dụng, đủ chi tiết để một ứng dụng khác — viết bằng Python, Java,
C#, Go hay bất cứ ngôn ngữ nào có gRPC — nói chuyện trực tiếp với hệ thống mà
không cần đi qua `s2t_qt`.

`s2t_qt` **không phải là một server**: nó là một client. Vì vậy "dùng dịch vụ
thông qua s2t_qt" trên thực tế nghĩa là **dùng đúng hợp đồng (contract) mà
s2t_qt đang dùng**, với `s2t_qt` là bản cài đặt tham chiếu đã chạy trong thực
tế. Mọi trường, mọi số hiệu trường và mọi quy tắc trong tài liệu này được đọc
thẳng từ mã nguồn `proto/` và `grpc/` của `s2t_qt`, nên nó khớp với thứ thật sự
chạy trên dây.

```
   client bên ngoài của bạn ─┐
                             ├──► gRPC :8700 ──► Triton (:8011) ──► GPU
   s2t_qt (ứng dụng Qt)    ──┘    (adapter)
```

Ba tài liệu đi cùng nhau:

| Tài liệu | Dành cho |
|---|---|
| [huong-dan-su-dung.md](huong-dan-su-dung.md) | người vận hành ứng dụng |
| [luong-hoat-dong.md](luong-hoat-dong.md) | người bảo trì mã nguồn `s2t_qt` |
| **danh-sach-api.md** (tài liệu này) | **người tích hợp hệ thống khác** |

---

## 1. Kết nối và xác thực

| Mục | Giá trị |
|---|---|
| Giao thức | gRPC trên HTTP/2 **cleartext (h2c)**, prior-knowledge — không TLS, không upgrade từ HTTP/1.1 |
| Địa chỉ mặc định | `192.168.1.47:8700` (cấu hình được) |
| Kiểu RPC | **unary** toàn bộ. Không có streaming ở bất kỳ RPC nào. |
| Đóng gói | proto3, `content-type: application/grpc+proto` |
| Nén | không nén; client gửi `grpc-encoding: identity` và `grpc-accept-encoding: identity` |
| Xác thực | header `authorization: Bearer <token>` trên **mọi** lệnh gọi |

Header mà `s2t_qt` gửi trên mỗi lệnh gọi (tham chiếu `grpc/GrpcChannel.cpp`):

```
:method       POST
:scheme       http
:path         /asr.ui.v1.ProductASRService/push_audio
:authority    <host:port>
content-type  application/grpc+proto
user-agent    s2t-qt/1.0 grpc-qt/1.0
te            trailers
grpc-encoding identity
grpc-accept-encoding identity
grpc-timeout  5000m
authorization Bearer <token>
```

`te: trailers` là **bắt buộc** — đó là cách server biết client đọc được
`grpc-status` ở trailer. Thân thông điệp theo đúng khung gRPC: 1 byte cờ nén +
4 byte độ dài big-endian + payload proto3.

Nếu adapter chạy không token thì bỏ header `authorization`; khi token sai hoặc
thiếu, mọi RPC trả về `UNAUTHENTICATED (16)`.

> **Lưu ý bảo mật.** Đường truyền là cleartext. Token đi qua mạng ở dạng rõ,
> nên chỉ dùng trong mạng nội bộ tin cậy hoặc bọc thêm một lớp TLS/VPN ở tầng
> dưới. Đừng ghi token vào log của client.

### 1.1 Kiểm tra nhanh trước khi tích hợp

`s2t_qt` có sẵn một phép thử không cần cài thêm gì:

```
s2t_qt --probe 192.168.1.47:8700 --token <token>
```

Nó gọi thật ba RPC — `get_model_status`, `list_sessions` và
`GetSpeakerRegistryStatus` — nên nó kiểm tra được **cả đường mạng lẫn token**,
và in ra danh sách model kèm độ trễ. Đây là cách nhanh nhất để tách bạch "mạng
hỏng" với "token sai" trước khi đổ lỗi cho mã của bạn.

Từ phía client mới, RPC rẻ nhất để thử là `get_model_status`: nó không cần
phiên, không cần tham số:

```
grpcurl -plaintext -proto asr_session.proto -H "authorization: Bearer $TOKEN" \
        192.168.1.47:8700 asr.ui.v1.ProductASRService/get_model_status
```

Tệp `.proto` để biên dịch nằm ở [phụ lục A](#phụ-lục-a--tệp-proto-tái-dựng).

---

## 2. Mã lỗi và chính sách thử lại

Mã trạng thái theo đúng chuẩn gRPC. Ba mã được coi là **lỗi truyền tải** và an
toàn để thử lại nguyên văn:

| Mã | Tên | Thử lại? | Ý nghĩa thực tế |
|---|---|---|---|
| 14 | `UNAVAILABLE` | **Có** | Không nối được, socket đứt, adapter đang khởi động lại |
| 4 | `DEADLINE_EXCEEDED` | **Có** | Hết hạn `grpc-timeout` |
| 1 | `CANCELLED` | **Có** | Kết nối bị huỷ giữa chừng |
| 13 | `INTERNAL` | **KHÔNG** | Xem cảnh báo bên dưới |
| 16 | `UNAUTHENTICATED` | Không | Token sai hoặc thiếu |
| 3 | `INVALID_ARGUMENT` | Không | Sai tham số; sửa yêu cầu rồi gọi lại |
| 5 | `NOT_FOUND` | Không | `session_id` không tồn tại |
| 9 | `FAILED_PRECONDITION` | Không | Ví dụ `edit_range_not_committed` |
| 10 | `ABORTED` | Không | Xung đột `base_revision` khi sửa văn bản |
| 8 | `RESOURCE_EXHAUSTED` | Không | Vượt trần kích thước thông điệp |

> ### Không bao giờ thử lại `INTERNAL` trên `push_audio`
>
> Adapter trả `INTERNAL` **đúng vào lúc nó có thể đã tiêu thụ gói audio đó
> rồi**. Thử lại một cách mù quáng sẽ làm nhân đôi chữ trong bản chép. Đây là
> ràng buộc mạnh nhất của toàn bộ API này; `s2t_qt` cài đặt nó ở
> `grpc::Status::isTransport()` và ở vòng lặp `SessionWorker::pushPacket()`.

Khi gặp lỗi truyền tải, cách làm đã kiểm chứng là: **bỏ hẳn kết nối TCP hiện
tại, mở lại, gửi lại đúng `seq` cũ** — chứ không chờ HTTP/2 tự phục hồi trên
socket đã hỏng. `s2t_qt` chờ 500 ms giữa hai lần thử.

`grpc-message` được percent-encode theo chuẩn gRPC, và thông điệp lỗi từ
adapter có tiếng Việt có dấu — nhớ giải mã trước khi hiển thị.

Một số client gRPC chỉ đọc `grpc-status` ở trailer. Adapter trả lỗi ở dạng
**trailers-only** (status nằm ngay trong HEADERS block đầu tiên, không có DATA
frame nào), nên hãy chắc thư viện của bạn xử lý được cả hai chỗ.

---

## 3. Định dạng audio

Áp dụng cho `push_audio` (gửi lên) và `get_audio_range` (nhận về).

| Mục | Giá trị |
|---|---|
| Mã hoá | PCM 16-bit little-endian, `audio_format = "s16le"` |
| Sample rate | 48000 Hz mặc định (khai báo trong `sample_rate`) |
| Số kênh | 1 (khai báo trong `channels`) |
| Kích thước gói khi ghi trực tiếp | **160 ms** mỗi `push_audio` |
| Kích thước gói khi phát lại tệp | **320 ms** — đúng bước nhảy diar gốc của pipeline |
| Đăng ký giọng (`EnrollSpeaker`) | tệp **WAV 16 kHz, 1 kênh**, có đủ header RIFF |

Vì sao 160 ms: đủ nhỏ để độ trễ hiển thị không thấy được, đủ lớn để không có
việc mạng hay protobuf nào chạy trên đường thu audio. Vì sao khi phát lại tệp
là 320 ms: nó giảm một nửa số RPC mà vẫn không vượt giới hạn nạp cố định của
model. Gói lớn hơn nhiều **không** làm nhanh hơn — nó chỉ làm trễ tăng.

Trường `vad_chunk_ms` phải bằng đúng độ dài gói đang gửi (160 hoặc 320).

Số byte một gói: `sample_rate * channels * 2 * ms / 1000`. Với 48 kHz mono
160 ms là 15360 byte.

---

## 4. Vòng đời một phiên

```
  start_session(config_json)                 ──►  session_id
      │
      ├─ push_audio(seq=1, reset=true,  pcm) ──►  văn bản streaming + diar
      ├─ push_audio(seq=2, reset=false, pcm) ──►  ...
      ├─ push_audio(seq=N, ...)              ──►  ...
      │       (song song, trên kênh khác)
      │       get_live_state(session_id)  mỗi 200 ms
      │
      └─ stop_session(session_id)            ──►  trạng thái chốt

  sau khi phiên kết thúc:
      get_review_state · get_audio_range · apply_text_edit · rename_speaker
      get_audit_history · get_pipeline_trace
      ListSessionSpeakers · SaveSessionSpeakers
```

Bốn quy tắc thứ tự **không được phá**, mỗi cái tương ứng một sự cố thật:

1. **`seq` bắt đầu từ 1 và tăng đều một đơn vị cho mỗi gói của phiên.** Adapter
   phát lại nguyên phản hồi đã lưu cho một `seq` nó từng xử lý — đây là thứ duy
   nhất làm cho việc thử lại sau `DEADLINE_EXCEEDED` an toàn trên một pipeline
   có trạng thái.
2. **Gói đầu tiên đặt `reset = true`**, các gói sau đặt `false`.
3. **`stop_session` phải được gửi từ cùng luồng/cùng hàng đợi với vòng đẩy
   audio**, sau gói cuối cùng. Gửi từ luồng khác thì nó có thể vượt mặt gói
   cuối và phiên bị chốt thiếu.
4. **Mở thiết bị thu trước, gọi `start_session` sau.** Đảo lại sẽ để một phiên
   rỗng treo trên server khi driver âm thanh lỗi.

### 4.1 Deadline nên đặt

Đây là các giá trị `s2t_qt` dùng, đã chạy thực tế:

| RPC | Deadline | Vì sao |
|---|---|---|
| `start_session` | 20 s | |
| `push_audio` | **5 s** | Ngắn có chủ ý: phát hiện đứt mạng sớm. Thử lại cùng `seq` là an toàn. |
| `get_live_state` | 20 s | |
| `get_review_state` | 20 s | |
| `stop_session` | **3600 s** | Nó rút cạn hàng đợi bền trên server; sau một lần nạp tệp nhanh, hàng đợi đó có thể còn vài phút audio. Đặt ngắn là chốt thiếu phiên. |
| `apply_text_edit`, `rename_speaker` | 60 s | |
| `get_audio_range`, `SaveSessionSpeakers` | 120 s | |
| `EnrollSpeaker` | **120 s** | Bao trọn một lượt `rebuild_db` trên **toàn bộ** giọng đã có, không chỉ giọng vừa ghi. |
| `get_pipeline_trace`, `ListSessionSpeakers` | 30 s | |
| `get_model_status`, `GetEnrollmentScript`, `GetSpeakerRegistryStatus` | 20 s | |

### 4.2 Nên dùng mấy kết nối

Một `Channel` gRPC ứng với một kết nối TCP. Đừng dồn mọi thứ vào một kênh:

- `get_live_state` **phình theo độ dài cuộc họp**. Dùng chung kênh với audio
  thì một lần tuần tự hoá trạng thái lớn sẽ nằm chắn trước một gói audio 160 ms.
- Một `EnrollSpeaker` được phép chạy tới 120 giây và sẽ chắn mọi thứ phía sau.

`s2t_qt` dùng **5 kênh**: một cho vòng đẩy audio, một cho poll trạng thái, và
ba lane dùng chung cho các RPC theo yêu cầu.

Trần kích thước phản hồi phía client nên đặt **64 MiB** (bản thân adapter đã
chặn ở 4 MiB của gRPC); cửa sổ nhận HTTP/2 mức kết nối nên nâng lên 8 MiB, nếu
không một `get_review_state` lớn sẽ nghẽn ở 64 KiB.

---

## 5. `ProductASRService` — 12 RPC

Tên gói: `asr.ui.v1`. Tên RPC của service này viết **snake_case**; service
đăng ký giọng viết PascalCase — bất đối xứng đó nằm trong hợp đồng, không phải
lỗi đánh máy.

### 5.1 `start_session`

```
/asr.ui.v1.ProductASRService/start_session
StartSessionRequest → StartSessionResponse
```

**Request**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `config_json` | `string` | JSON dạng compact chứa cấu hình phiên — xem [mục 7](#7-nội-dung-config_json) |

**Response**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | Định danh phiên, dùng cho mọi RPC sau |
| 2 | `stream_id` | `int64` | Định danh luồng nội bộ của adapter |
| 3 | `state_version` | `uint64` | Số hiệu phiên bản trạng thái |
| 4 | `state` | `SessionState` | Trạng thái rỗng ban đầu |

### 5.2 `push_audio`

```
/asr.ui.v1.ProductASRService/push_audio
PushAudioRequest → PushAudioResponse
```

**Request**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | Từ `start_session` |
| 2 | `pcm` | `bytes` | Audio thô, s16le |
| 3 | `sample_rate` | `uint32` | Ví dụ 48000 |
| 4 | `channels` | `uint32` | Ví dụ 1 |
| 5 | `audio_format` | `string` | `"s16le"` |
| 6 | `reset` | `bool` | `true` **chỉ** ở gói đầu tiên |
| 7 | `vad_chunk_ms` | `uint32` | Độ dài gói: 160 (trực tiếp) hoặc 320 (phát lại tệp) |
| 8 | `seq` | `uint64` | Bộ đếm đơn điệu theo phiên, **bắt đầu từ 1**. Khoá idempotency. |

**Response** — đây là thông điệp giàu thông tin nhất của API:

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `stream_id` | `int64` | |
| 3 | `state_version` | `uint64` | |
| 4 | `events` | `EventFlags` | `streaming` / `correction` / `final` |
| 5 | `source_seen_sec` | `double` | Số giây audio server đã **nhận và ghi bền** — dùng để tính hàng đợi phía server |
| 6 | `speech_seen_sec` | `double` | Số giây thực sự là tiếng nói sau VAD |
| 7 | `streaming_text` | `string` | Văn bản tạm của gói này |
| 8 | `text` | `string` | Văn bản đã ổn định |
| 9 | `itn_text` | `string` | Sau chuẩn hoá số/ngày tháng (ITN) |
| 10 | `itn_full_text` | `string` | Toàn bộ phiên sau ITN |
| 11 | `itn_correction_text` | `string` | Phần ITN vừa được sửa |
| 12 | `asr_words` | `repeated Word` | Từng từ kèm mốc thời gian |
| 13 | `asr_confidence` | `float` | Độ tin cậy trung bình của gói |
| 14 | `asr_word_confidence` | `repeated float` (packed) | Độ tin cậy từng từ |
| 15 | `speaker` | `string` | Nhãn diar, ví dụ `speaker_0` |
| 16 | `speaker_prob` | `float` | |
| 17 | `verified_name` | `string` | Tên thật nếu CAM++ nhận ra |
| 18 | `verify_score` | `float` | |
| 19 | `chunk_start_ms` | `int64` | |
| 20 | `chunk_start_sec` | `double` | |
| 21 | `chunk_end_sec` | `double` | |
| 22 | `diarization` | `Diarization` | Ma trận điểm diar thô |
| 23 | `correction` | `CorrectionUpdate` | Bản vá hồi tố cho văn bản đã hiển thị |
| 24 | `timing` | `Timing` | Thời gian từng chặng phía server |

> **Hai hàng đợi là hai con số khác nhau.** `source_seen_sec` là thứ server đã
> **ghi bền**, không phải thứ nó đã xử lý xong. Hàng đợi phía server =
> `(tổng giây đã gửi) − source_seen_sec`. Hàng đợi phía client là audio đã thu
> mà chưa được ACK. Đừng gộp hai con số này lại; khi mạng chập chờn chúng lệch
> nhau rất xa và người vận hành cần thấy cả hai.

### 5.3 `get_live_state`

```
/asr.ui.v1.ProductASRService/get_live_state
SessionRequest → StateResponse
```

**Request**: `1 session_id (string)`.

Poll mỗi **200 ms** trên một kênh riêng. Đây là nguồn dữ liệu để vẽ toàn bộ
giao diện trực tiếp; `push_audio` trả về phần tăng thêm, còn RPC này trả về
bức tranh đầy đủ.

**Response** (`StateResponse`)

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `stream_id` | `int64` | |
| 3 | `state_version` | `uint64` | Tăng khi trạng thái đổi |
| 4 | `state` | `SessionState` | Toàn bộ nội dung hiển thị |
| 5 | `transcript_revision` | `uint64` | **Dùng làm `base_revision` khi sửa** |
| 6 | `transcript_final` | `bool` | |
| 7 | `commit_boundary_sec` | `double` | Mốc chốt: chữ sau mốc này chưa được phép sửa |

### 5.4 `get_review_state`

```
/asr.ui.v1.ProductASRService/get_review_state
ReviewRequest → StateResponse
```

**Request**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `has_view_start_sec` | `bool` | Có giới hạn đầu cửa sổ không |
| 3 | `view_start_sec` | `double` | |
| 4 | `has_view_end_sec` | `bool` | Có giới hạn cuối cửa sổ không |
| 5 | `view_end_sec` | `double` | |

Cặp `has_*` tồn tại vì proto3 không phân biệt được "không đặt" với "đặt bằng
0" — mà 0 giây là một mốc hợp lệ. Muốn lấy cả phiên thì để cả hai `has_*` bằng
`false`.

Với cuộc họp dài, **hãy lấy theo cửa sổ**. `s2t_qt` cuốn theo từng cửa sổ và
dừng ở 200 cửa sổ (khoảng 33 giờ) để một phiên hỏng không kéo client đi mãi.

### 5.5 `get_audio_range`

```
/asr.ui.v1.ProductASRService/get_audio_range
AudioRangeRequest → AudioRangeResponse
```

**Request**: `1 session_id (string)`, `2 start_sec (double)`, `3 end_sec (double)`.

**Response**

| # | Trường | Kiểu |
|---|---|---|
| 1 | `session_id` | `string` |
| 2 | `pcm` | `bytes` (s16le) |
| 3 | `sample_rate` | `uint32` |
| 4 | `channels` | `uint32` |
| 5 | `audio_format` | `string` |
| 6 | `start_sec` | `double` |
| 7 | `end_sec` | `double` |
| 8 | `total_sec` | `double` |

Khoảng trả về **có thể bị cắt** so với khoảng yêu cầu — luôn đọc `start_sec` /
`end_sec` trong phản hồi thay vì giả định chúng bằng thứ đã gửi đi.

### 5.6 `apply_text_edit`

```
/asr.ui.v1.ProductASRService/apply_text_edit
TextEditRequest → ReviewEditResponse
```

**Request**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `base_revision` | `uint64` | Revision client đang thấy — **khoá đồng thuận lạc quan** |
| 3 | `start_sec` | `double` | Đầu khoảng bị thay |
| 4 | `end_sec` | `double` | Cuối khoảng bị thay |
| 5 | `replacement_words` | `repeated Word` | Nội dung mới |
| 6 | `editor_id` | `string` | **Người chịu trách nhiệm** — vào nhật ký kiểm toán |
| 7 | `note` | `string` | Lý do sửa |

**Response** (`ReviewEditResponse`): `1 session_id`, `2 transcript
(CanonicalTranscript)`, `3 state (SessionState)`.

Hai luật bắt buộc:

- **`base_revision` là bắt buộc.** Server từ chối nếu revision đã nhích; client
  phải tải lại rồi cho người dùng sửa lại, không được ghi đè âm thầm.
- **Không sửa quá `commit_boundary_sec`.** Chữ sau mốc chốt vẫn còn là kết quả
  tạm; server trả `edit_range_not_committed`. Hãy chặn ngay ở giao diện thay vì
  để người dùng gõ vào một thứ chắc chắn bị từ chối.

`editor_id` **không nên được nhớ giữa các lần chạy**. Nó ghi lại ai chịu trách
nhiệm cho một thay đổi; một ô tự điền lại sẽ ghi công việc của người này dưới
tên người khác.

### 5.7 `stop_session`

```
/asr.ui.v1.ProductASRService/stop_session
SessionRequest → StopSessionResponse
```

**Request**: `1 session_id (string)`.

**Response**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `stream_id` | `int64` | |
| 3 | `state_version` | `uint64` | |
| 4 | `events` | `EventFlags` | |
| 5 | `result` | `PushAudioResponse` | Kết quả của phần audio còn lại |
| 6 | `state` | `SessionState` | Trạng thái chốt |

Deadline dài (1 giờ) là bắt buộc — xem [mục 4.1](#41-deadline-nên-đặt).

### 5.8 `list_sessions`

```
/asr.ui.v1.ProductASRService/list_sessions
ListSessionsRequest → ListSessionsResponse
```

**Request**: `1 limit (uint32)`, `2 cursor (string)`.

**Response**: `1 sessions (repeated SessionSummary)`, `2 next_cursor (string)`.

`SessionSummary`: `1 session_id`, `2 title`, `3 created_at (double, epoch
giây)`, `4 updated_at`, `5 duration_sec`, `6 final (bool)`, `7 running (bool)`,
`8 participants (repeated string)`, `9 security_level`, `10 mode`.

Phân trang bằng `next_cursor`: rỗng nghĩa là hết.

### 5.9 `rename_speaker`

```
/asr.ui.v1.ProductASRService/rename_speaker
RenameSpeakerRequest → ReviewEditResponse
```

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `from_speaker` | `string` | Nhãn diar hiện tại, ví dụ `speaker_2` |
| 3 | `to_speaker` | `string` | Nhãn mới |
| 4 | `verified_name` | `string` | **Luôn được áp dụng, kể cả khi rỗng** |
| 5 | `editor_id` | `string` | |
| 6 | `note` | `string` | |

`verified_name` rỗng **không** có nghĩa "giữ nguyên" — proto3 không phân biệt
được "không đặt" với "đặt bằng rỗng", nên rỗng là một lệnh **xoá tên** có chủ
ý. Muốn giữ tên cũ thì phải gửi lại đúng tên cũ.

### 5.10 `get_pipeline_trace`

```
/asr.ui.v1.ProductASRService/get_pipeline_trace
PipelineTraceRequest → PipelineTraceResponse
```

**Request**: `1 session_id`, `2 after_seq (uint64)`, `3 limit (uint32)`,
`4 stages (repeated string)`.

**Response**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_id` | `string` | |
| 2 | `events` | `repeated PipelineTraceEvent` | |
| 3 | `next_seq` | `uint64` | Truyền lại vào `after_seq` để lấy tiếp |
| 4 | `has_more` | `bool` | |
| 5 | `enabled` | `bool` | **Trace có được bật cho phiên này không** |
| 6 | `truncated` | `bool` | Bản ghi đã bị cắt vì vượt `max_bytes` |
| 7 | `max_bytes` | `uint64` | |

`PipelineTraceEvent`: `1 seq`, `2 ts (double)`, `3 stage`, `4 event`,
`5 audio_start_sec`, `6 audio_end_sec`, `7 payload_json (string)`.

Trace chỉ có nếu phiên được tạo với `"pipeline_trace": true` trong
`config_json` — bật sau khi phiên đã chạy thì không có tác dụng hồi tố. Khi
`enabled = false`, danh sách rỗng là đúng chứ không phải lỗi.

### 5.11 `get_audit_history`

```
/asr.ui.v1.ProductASRService/get_audit_history
AuditHistoryRequest → AuditHistoryResponse
```

**Request**: `1 session_id`, `2 limit (uint32)`.

**Response**: `1 session_id`, `2 events (repeated AuditEvent)`, với
`AuditEvent` = `1 ts (double)`, `2 event (string)`, `3 payload_json (string)`.

Đây là nơi mọi `apply_text_edit` và `rename_speaker` được ghi lại kèm
`editor_id`. Nếu hệ thống của bạn có yêu cầu truy vết, hãy đọc từ đây chứ đừng
tự dựng lại lịch sử ở phía client.

### 5.12 `get_model_status`

```
/asr.ui.v1.ProductASRService/get_model_status
ModelStatusRequest (rỗng) → ModelStatusResponse
```

**Response**: `1 models (repeated ModelStatusEntry)`, với `ModelStatusEntry` =
`1 name`, `2 version`, `3 state`.

Đọc thẳng từ Triton. Đây là RPC rẻ nhất và không cần phiên, nên nó là lựa chọn
tốt cho health-check và cho việc kiểm tra token.

---

## 6. `SpeakerRegistryService` — 5 RPC

Cùng host, cùng cổng, cùng token với `ProductASRService`; một kênh phục vụ cả
hai. Tên RPC ở đây viết **PascalCase**.

### 6.1 `GetEnrollmentScript`

```
/asr.ui.v1.SpeakerRegistryService/GetEnrollmentScript
GetEnrollmentScriptRequest (rỗng) → GetEnrollmentScriptResponse
```

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `script_text` | `string` | Đoạn văn để người đăng ký đọc to |
| 2 | `sample_rate` | `uint32` | Sample rate WAV cần gửi (16000) |
| 3 | `recommended_duration_sec` | `double` | |
| 4 | `target_segments` | `uint32` | Số đoạn giọng mong muốn |

Luôn gọi RPC này trước khi ghi âm, đừng hard-code kịch bản: server là nơi định
nghĩa yêu cầu chất lượng mẫu.

### 6.2 `EnrollSpeaker`

```
/asr.ui.v1.SpeakerRegistryService/EnrollSpeaker
EnrollSpeakerRequest → EnrollSpeakerResponse
```

**Request**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `display_name` | `string` | Tên hiển thị của người nói |
| 2 | `wav` | `bytes` | **Tệp WAV đầy đủ** (RIFF header), 16 kHz mono |
| 3 | `editor_id` | `string` | Người thực hiện đăng ký |
| 4 | `note` | `string` | |
| 5 | `allow_below_policy` | `bool` | Cho phép lưu mẫu chưa đạt chuẩn |

**Response**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `ok` | `bool` | |
| 2 | `error` | `string` | |
| 3 | `speaker_id` | `string` | |
| 4 | `raw_seconds` | `double` | Độ dài mẫu thô |
| 5 | `speech_seconds_after_vad` | `double` | Phần thực sự là tiếng nói |
| 6 | `segments_enrolled` | `uint32` | |
| 7 | `target_segments` | `uint32` | |
| 8 | `warning` | `string` | |
| 9 | `db_mtime` | `double` | Thời điểm CSDL CAM++ được ghi lại |

> `allow_below_policy = true` **không bao giờ im lặng**: mẫu được lưu với cờ
> `policy_compliant = false` và phản hồi mang theo `warning`, để người nói đó
> vẫn hiện rõ là cần đăng ký lại tử tế. Hãy hiển thị `warning` này, đừng nuốt.

Đây là RPC chậm nhất của API: nó kéo theo một lượt `rebuild_db` trên **toàn
bộ** giọng đã có. Đặt deadline 120 giây và đừng gọi nó trên kênh đang dùng cho
việc khác.

### 6.3 `ListSessionSpeakers`

```
/asr.ui.v1.SpeakerRegistryService/ListSessionSpeakers
ListSessionSpeakersRequest → ListSessionSpeakersResponse
```

**Request**: `1 session_id (string)`.
**Response**: `1 session_id`, `2 speakers (repeated SessionSpeakerEntry)`.

`SessionSpeakerEntry`:

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `session_speaker_id` | `string` | |
| 2 | `diar_slots` | `repeated string` | Các nhãn diar được gộp vào người này |
| 3 | `verified_name` | `string` | |
| 4 | `score` | `double` | |
| 5 | `windows` | `uint32` | |
| 6 | `created_at` | `double` | |
| 7 | `updated_at` | `double` | |
| 8 | `status` | `string` | `pending` / `session_only` / `global_shared` / `publish_failed` |
| 9 | `has_evidence` | `bool` | |
| 10 | `evidence` | `SessionSpeakerEvidence` | |
| 11 | `published_name` | `string` | |
| 12 | `published_at` | `double` | |
| 13 | `publish_error` | `string` | |

`SessionSpeakerEvidence`: `1 total_speech_sec`, `2 span_count (uint32)`,
`3 staged_at (double)`, `4 source_verified_name (string)`.

### 6.4 `SaveSessionSpeakers`

```
/asr.ui.v1.SpeakerRegistryService/SaveSessionSpeakers
SaveSessionSpeakersRequest → SaveSessionSpeakersResponse
```

**Request**: `1 session_id`, `2 selections (repeated SpeakerSelection)`,
`3 editor_id`.

`SpeakerSelection`: `1 session_speaker_id (string)`, `2 destination (enum)`,
`3 global_name (string)`.

`SpeakerDestination`:

| Giá trị | Tên | Ý nghĩa |
|---|---|---|
| 0 | `SPEAKER_DESTINATION_UNSPECIFIED` | Không hợp lệ |
| 1 | `SESSION_ONLY` | Giữ danh tính này **trong phạm vi phiên**; không đụng CSDL toàn cục |
| 2 | `GLOBAL_SHARED` | Công bố bằng chứng đã gom vào CSDL CAM++ toàn cục (kéo theo rebuild) |

`global_name` chỉ dùng với `GLOBAL_SHARED`; để rỗng nghĩa là dùng
`verified_name` của chính mục đó.

**Response**: `1 session_id`, `2 results (repeated SaveSpeakerResult)`, với
`SaveSpeakerResult` = `1 session_speaker_id`, `2 ok (bool)`, `3 status`,
`4 error`, `5 segments_enrolled (uint32)`.

Kết quả là **theo từng người**: một phần thành công và một phần thất bại trong
cùng một lệnh gọi là chuyện bình thường. Duyệt hết mảng `results`, đừng chỉ
nhìn mã trạng thái gRPC.

### 6.5 `GetSpeakerRegistryStatus`

```
/asr.ui.v1.SpeakerRegistryService/GetSpeakerRegistryStatus
GetSpeakerRegistryStatusRequest → GetSpeakerRegistryStatusResponse
```

**Request**: `1 session_id (string)` — để rỗng nếu chỉ cần trạng thái toàn cục.

**Response**

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `global_db_mtime` | `double` | |
| 2 | `global_db_revision` | `string` | |
| 3 | `global_speaker_count` | `uint32` | |
| 4 | `sidecar_reachable` | `bool` | CAM++ sidecar có sống không |
| 5 | `session_id` | `string` | |
| 6 | `session_pending_count` | `uint32` | |
| 7 | `session_published_count` | `uint32` | |
| 8 | `session_failed_count` | `uint32` | |
| 9 | `global_speaker_names` | `repeated string` | |
| 10 | `speakers_below_policy` | `repeated SpeakerBelowPolicy` | |

`SpeakerBelowPolicy`: `1 spk_id`, `2 spk_name`, `3 sample_count (uint32)`,
`4 longest_sample_sec (double)`, `5 reason (string)`, `6 kind (string)`.

`kind` nhận `legacy` / `urgent` / `other`. Nó tồn tại vì trên một máy vừa di
trú, **mọi** giọng đều dưới chuẩn; nếu không tách `legacy` ra thì một ca
`urgent` thật sẽ chìm nghỉm giữa danh sách.

---

## 7. Nội dung `config_json`

Chuỗi JSON compact truyền trong `start_session`. Mọi khoá đều tuỳ chọn:

| Khoá | Kiểu | Ý nghĩa |
|---|---|---|
| `pipeline_trace` | `bool` | Bật lưu vết từng chặng cho phiên này. Chỉ ghi khi bật. |
| `source_total_sec` | `number` | Tổng độ dài nguồn khi phát lại tệp — cho thanh tiến trình |
| `expected_speakers` | `array<string>` | **Ba trạng thái — xem bên dưới** |
| `mode` | `string` | Chế độ cuộc họp |
| `session_title` | `string` | Tiêu đề |
| `participants` | `array<string>` | Danh sách người dự |
| `security_level` | `string` | Mức bảo mật |

> ### `expected_speakers` có ba trạng thái, không phải hai
>
> | Trạng thái | Cách biểu diễn | Ý nghĩa |
> |---|---|---|
> | Không giới hạn | **Không có khoá** `expected_speakers` | Đối chiếu với toàn bộ CSDL giọng |
> | Giới hạn theo danh sách | `["An", "Bình"]` | Chỉ đối chiếu với những người này |
> | Không gán tên ai | `[]` (mảng rỗng tường minh) | Không gán tên thật cho bất kỳ ai |
>
> Mảng rỗng **không** đồng nghĩa với việc bỏ khoá. Gộp hai cái làm một là đổi
> nghĩa của phiên: "không nhận diện ai" sẽ âm thầm biến thành "đối chiếu toàn
> bộ danh bạ". Client của bạn phải giữ được sự phân biệt này — trong JSON,
> nghĩa là phân biệt giữa *bỏ khoá* và *ghi khoá với giá trị `[]`*.

Ví dụ:

```json
{"pipeline_trace":true,"session_title":"Giao ban tuần 34","participants":["An","Bình"],"security_level":"internal","expected_speakers":["An","Bình"]}
```

---

## 8. Kiểu dữ liệu dùng chung

### `Word`

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `w` | `string` | Chữ |
| 2 | `c` | `float` | Độ tin cậy 0..1 |
| 3 | `start_sec` | `double` | |
| 4 | `end_sec` | `double` | |
| 5 | `speaker` | `string` | |

### `Phrase`

`1 text`, `2 avg_conf (float)`, `3 is_low_conf (bool)`, `4 start_sec`,
`5 end_sec`, `6 words (repeated Word)`.

### `DisplayRow`

Một dòng hiển thị: một người nói, một quãng thời gian liền mạch.

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `row_id` | `string` | |
| 2 | `speaker` | `string` | Nhãn diar |
| 3 | `speaker_prob` | `float` | |
| 4 | `verified_name` | `string` | Tên thật nếu đã xác thực |
| 5 | `start_sec` | `double` | |
| 6 | `end_sec` | `double` | |
| 8 | `merged_text` | `string` | Văn bản đã ổn định |
| 10 | `updating_text` | `string` | Phần đuôi còn đang đổi |
| 11 | `stable_token_count` | `uint32` | |
| 12 | `is_provisional` | `bool` | |
| 13 | `phrases` | `repeated Phrase` | |
| 14 | `display_tokens` | `repeated Word` | |
| 15 | `updating_tokens` | `repeated Word` | |

**Số 7 và 9 là `reserved`** (`itn_text` / `stable_text` đã bỏ). Một server cũ
vẫn có thể gửi chúng; client phải **bỏ qua** chứ không được coi là dữ liệu
hỏng. Đừng tái sử dụng hai số này cho trường mới.

### `SessionState`

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `title` | `string` | |
| 2 | `conf_threshold_pct` | `uint32` | Ngưỡng tin cậy, mặc định tương ứng 0.75 |
| 3 | `rows` | `repeated DisplayRow` | Nội dung chính |
| 4 | `provisional_rows` | `repeated DisplayRow` | Dòng chưa chốt |
| 5 | `speaker_ids` | `repeated string` | |
| 6 | `highlights` | `repeated Highlight` | Do server chọn |
| 7 | `n_phrases` | `uint32` | |
| 8 | `n_low` | `uint32` | Số cụm dưới ngưỡng tin cậy |
| 9 | `amp_trace` | `repeated float` (packed) | Đường biên độ để vẽ dạng sóng |
| 10 | `amp_trace_step_sec` | `double` | |
| 11 | `source_total_sec` | `double` | |
| 12 | `source_seen_sec` | `double` | |
| 13 | `speech_seen_sec` | `double` | |
| 14 | `wall_elapsed_sec` | `double` | |
| 15 | `playhead_ratio` | `double` | |
| 16 | `done` | `bool` | |
| 17 | `ts` | `double` | |
| 18 | `last_asr_chunk_ms` | `uint32` | |
| 19 | `infer_p50_ms` | `double` | |
| 20 | `infer_p95_ms` | `double` | |
| 21 | `latency` | `LatencySummary` | |

### `Highlight`

`1 start_sec (double)`, `2 speaker`, `3 text`, `4 conf_pct (uint32)`.

### `EventFlags`

`1 streaming (bool)`, `2 correction (bool)`, `3 final (bool)`.

### `Diarization`

`1 flat_scores (repeated float, packed)`, `2 shape (repeated int32, packed)`,
`3 subframe_start_ms (repeated int64, packed)`,
`4 subframe_end_ms (repeated int64, packed)`.

Ma trận điểm được làm phẳng; `shape` cho biết cách gấp lại.

### `CorrectionUpdate`

Bản vá hồi tố: model sửa lại văn bản đã hiển thị khi có thêm ngữ cảnh.

| # | Trường | Kiểu | Ý nghĩa |
|---|---|---|---|
| 1 | `text` | `string` | |
| 2 | `full_text` | `string` | |
| 3 | `committed_text` | `string` | Phần đã chốt, không đổi nữa |
| 4 | `tail_text` | `string` | Phần đuôi còn có thể đổi |
| 5 | `merged_words` | `repeated Word` | |
| 6 | `updated_indices` | `repeated int32` (packed) | Vị trí các từ vừa đổi |
| 7 | `update_start_sec` | `double` | |
| 8 | `update_end_sec` | `double` | |
| 9 | `commit_boundary_sec` | `double` | |
| 10 | `num_committed` | `uint32` | |
| 11 | `num_tail` | `uint32` | |
| 12 | `itn_ms` | `double` | |
| 13 | `merge_ms` | `double` | |

### `CanonicalTranscript`

`1 revision (uint64)`, `2 final (bool)`, `3 commit_boundary_sec (double)`,
`4 text (string)`, `5 words (repeated Word)`.

### `Timing`

`1 client_prepare_ms`, `2 client_wait_ms`, `3 asr_ms`, `4 diar_ms`,
`5 verify_ms`, `6 itn_ms`, `7 vad_ms`, `8 denoise_ms` — tất cả `double`.

Lấy độ trễ RPC đo được trừ đi `client_prepare_ms + client_wait_ms` sẽ ra phần
mạng + gRPC + tuần tự hoá của adapter, mà **không** phụ thuộc vào việc đồng hồ
hai máy có khớp nhau hay không.

### `LatencySummary`

`1 client (LatencyClient)`, `2 server (LatencyServer)`, `3 ui (LatencyUi)`.

- `LatencyClient`: `1 prepare_p50`, `2 prepare_p95`, `3 wait_p50`,
  `4 wait_p95`, `5 parse_p50`, `6 parse_p95`, `7 e2e_p50`, `8 e2e_p95`,
  `9 overhead_vs_server_sum_p50`, `10 overhead_vs_server_sum_p95`.
- `LatencyServer`: `1 asr_p50`, `2 asr_p95`, `3 diar_p50`, `4 diar_p95`,
  `5 verify_p50`, `6 verify_p95`, `7 itn_p50`, `8 itn_p95`, `9 vad_p50`,
  `10 vad_p95`, `11 denoise_p50`, `12 denoise_p95`, `13 sum_p50`, `14 sum_p95`.
- `LatencyUi`: `1 on_chunk_total_p50`, `2 on_chunk_total_p95`.

Tất cả đều `double`, đơn vị mili giây.

---

## 9. Ví dụ tích hợp

### 9.1 Python

```python
import grpc, wave, asr_session_pb2 as pb, asr_session_pb2_grpc as rpc

TARGET, TOKEN = "192.168.1.47:8700", "<token>"
META = (("authorization", f"Bearer {TOKEN}"),)

# Kênh riêng cho audio, kênh riêng cho poll trạng thái - xem muc 4.2
audio_ch = grpc.insecure_channel(TARGET)
state_ch = grpc.insecure_channel(TARGET)
audio = rpc.ProductASRServiceStub(audio_ch)
state = rpc.ProductASRServiceStub(state_ch)

started = audio.start_session(
    pb.StartSessionRequest(config_json='{"session_title":"Demo"}'),
    timeout=20, metadata=META)
sid = started.session_id

PACKET_MS, RATE = 160, 48000
packet_bytes = RATE * 1 * 2 * PACKET_MS // 1000
seq = 0
with wave.open("input_48k_mono_s16le.wav", "rb") as w:
    while True:
        pcm = w.readframes(packet_bytes // 2)
        if not pcm:
            break
        seq += 1
        req = pb.PushAudioRequest(
            session_id=sid, pcm=pcm, sample_rate=RATE, channels=1,
            audio_format="s16le", reset=(seq == 1),
            vad_chunk_ms=PACKET_MS, seq=seq)
        while True:                                  # vong thu lai
            try:
                audio.push_audio(req, timeout=5, metadata=META)
                break
            except grpc.RpcError as e:
                # CHI thu lai loi truyen tai. INTERNAL thi KHONG.
                if e.code() not in (grpc.StatusCode.UNAVAILABLE,
                                    grpc.StatusCode.DEADLINE_EXCEEDED,
                                    grpc.StatusCode.CANCELLED):
                    raise
                audio_ch.close()                     # mo lai ket noi moi
                audio_ch = grpc.insecure_channel(TARGET)
                audio = rpc.ProductASRServiceStub(audio_ch)

final = audio.stop_session(pb.SessionRequest(session_id=sid),
                           timeout=3600, metadata=META)
for row in final.state.rows:
    print(row.verified_name or row.speaker, ":", row.merged_text)
```

Điểm dễ sai nhất trong đoạn trên là vòng thử lại: `req` được tạo **một lần**
ngoài vòng lặp, nên lần gửi lại mang đúng `seq` cũ. Nếu tăng `seq` trong vòng
lặp thử lại, phiên sẽ có lỗ hoặc chữ bị nhân đôi.

### 9.2 C++ — dùng thẳng mã của `s2t_qt`

Nếu ứng dụng của bạn cũng là Qt/C++, không cần thư viện gRPC làm gì: chép hai
thư mục `proto/` và `grpc/` rồi dùng `AsrClient`. Cả hai không phụ thuộc gì
ngoài Qt Core — không protobuf, không gRPC, không protoc.

```cpp
AsrClient client(QStringLiteral("192.168.1.47:8700"), token);

asr::StartSessionRequest start;
start.configJson = QStringLiteral(R"({"session_title":"Demo"})");
asr::StartSessionResponse started;
if (!client.startSession(start, &started, 20000).ok())
    return;

asr::PushAudioRequest push;
push.sessionId = started.sessionId;
push.pcm       = packet;          // 160 ms s16le
push.sampleRate = 48000;
push.channels   = 1;
push.audioFormat = QStringLiteral("s16le");
push.reset  = (seq == 1);
push.vadChunkMs = 160;
push.seq    = seq;
asr::PushAudioResponse ack;
const grpc::Status st = client.pushAudio(push, &ack, 5000);
if (!st.ok() && st.isTransport()) {
    client.reset();               // dial lai, gui lai dung seq nay
}
```

`AsrClient` là **blocking** và một đối tượng thuộc về đúng một luồng. Đó là
lựa chọn có chủ ý: mọi bên gọi đều đã có luồng riêng, nên một socket đồng bộ
làm cả tầng giao thức gọn hơn nhiều so với một máy trạng thái callback.

### 9.3 Thử mà không cần server thật

`tools/mock_adapter.js` là một server HTTP/2 thuần Node, không phụ thuộc gì,
trả về đúng hình dạng thông điệp của adapter:

```
node tools/mock_adapter.js 18700
s2t_qt --selftest-net 127.0.0.1:18700
```

Client của bạn cũng trỏ vào cổng đó được. Nó cố tình trả về một payload ~2 MB
(để ép chạy `WINDOW_UPDATE` thật), phản hồi lỗi dạng trailers-only, và một
`grpc-message` tiếng Việt percent-encode — ba thứ hay làm vỡ một client mới
viết.

---

## 10. Những lỗi tích hợp thường gặp

| Triệu chứng | Nguyên nhân |
|---|---|
| Chữ bị nhân đôi trong bản chép | Thử lại `push_audio` sau `INTERNAL`, hoặc tăng `seq` khi gửi lại |
| Phiên bị chốt thiếu vài phút cuối | Deadline `stop_session` quá ngắn |
| Bản chép có lỗ ở giữa | `seq` nhảy cóc, hoặc gửi `reset=true` ở gói không phải gói đầu |
| Mọi RPC trả `UNAUTHENTICATED` | Thiếu `authorization`, hoặc token có khoảng trắng thừa |
| Gói audio bị chắn sau một RPC dài | Dồn mọi RPC vào một kênh — xem [mục 4.2](#42-nên-dùng-mấy-kết-nối) |
| Phản hồi lớn treo giữa chừng | Không nâng cửa sổ nhận HTTP/2 mức kết nối (nghẽn ở 64 KiB) |
| "Nhận diện được cả người ngoài danh sách" | Gửi `expected_speakers: []` nhưng thư viện JSON bỏ mảng rỗng đi |
| Tên người nói tự nhiên biến mất | Gửi `rename_speaker` với `verified_name` rỗng |
| Sửa văn bản bị từ chối | Sửa vượt `commit_boundary_sec`, hoặc `base_revision` đã cũ |
| Không thấy trace nào | Phiên không được tạo với `"pipeline_trace": true` |
| Không đọc được lỗi (chuỗi rỗng) | Chỉ đọc trailer, bỏ qua phản hồi trailers-only |

---

## Phụ lục A — tệp `.proto` tái dựng

Dưới đây là hợp đồng ở dạng `.proto`, **tái dựng từ các struct chép tay trong
`s2t_qt/proto/`** (số hiệu trường và kiểu dữ liệu lấy nguyên văn từ đó). Nó đủ
để `protoc` sinh mã cho một client mới.

Tệp gốc có thẩm quyền cao nhất vẫn là `ui_client/asr_session.proto` và
`ui_client/speaker_registry.proto` trong kho `s2t-dgpu`. Nếu hai bên khác nhau
thì tệp gốc đúng — và đó cũng là dấu hiệu tài liệu này cần được cập nhật.

```protobuf
// asr_session.proto
syntax = "proto3";
package asr.ui.v1;

service ProductASRService {
  rpc start_session       (StartSessionRequest)  returns (StartSessionResponse);
  rpc push_audio          (PushAudioRequest)     returns (PushAudioResponse);
  rpc get_live_state      (SessionRequest)       returns (StateResponse);
  rpc get_review_state    (ReviewRequest)        returns (StateResponse);
  rpc get_audio_range     (AudioRangeRequest)    returns (AudioRangeResponse);
  rpc apply_text_edit     (TextEditRequest)      returns (ReviewEditResponse);
  rpc stop_session        (SessionRequest)       returns (StopSessionResponse);
  rpc list_sessions       (ListSessionsRequest)  returns (ListSessionsResponse);
  rpc rename_speaker      (RenameSpeakerRequest) returns (ReviewEditResponse);
  rpc get_pipeline_trace  (PipelineTraceRequest) returns (PipelineTraceResponse);
  rpc get_audit_history   (AuditHistoryRequest)  returns (AuditHistoryResponse);
  rpc get_model_status    (ModelStatusRequest)   returns (ModelStatusResponse);
}

message StartSessionRequest { string config_json = 1; }
message SessionRequest      { string session_id  = 1; }

message PushAudioRequest {
  string session_id   = 1;
  bytes  pcm          = 2;
  uint32 sample_rate  = 3;
  uint32 channels     = 4;
  string audio_format = 5;
  bool   reset        = 6;
  uint32 vad_chunk_ms = 7;
  uint64 seq          = 8;
}

message ReviewRequest {
  string session_id         = 1;
  bool   has_view_start_sec = 2;
  double view_start_sec     = 3;
  bool   has_view_end_sec   = 4;
  double view_end_sec       = 5;
}

message AudioRangeRequest {
  string session_id = 1;
  double start_sec  = 2;
  double end_sec    = 3;
}

message AudioRangeResponse {
  string session_id   = 1;
  bytes  pcm          = 2;
  uint32 sample_rate  = 3;
  uint32 channels     = 4;
  string audio_format = 5;
  double start_sec    = 6;
  double end_sec      = 7;
  double total_sec    = 8;
}

message Word {
  string w         = 1;
  float  c         = 2;
  double start_sec = 3;
  double end_sec   = 4;
  string speaker   = 5;
}

message Phrase {
  string        text        = 1;
  float         avg_conf    = 2;
  bool          is_low_conf = 3;
  double        start_sec   = 4;
  double        end_sec     = 5;
  repeated Word words       = 6;
}

message DisplayRow {
  reserved 7, 9;                      // itn_text, stable_text da bo
  string          row_id             = 1;
  string          speaker            = 2;
  float           speaker_prob       = 3;
  string          verified_name      = 4;
  double          start_sec          = 5;
  double          end_sec            = 6;
  string          merged_text        = 8;
  string          updating_text      = 10;
  uint32          stable_token_count = 11;
  bool            is_provisional     = 12;
  repeated Phrase phrases            = 13;
  repeated Word   display_tokens     = 14;
  repeated Word   updating_tokens    = 15;
}

message Highlight {
  double start_sec = 1;
  string speaker   = 2;
  string text      = 3;
  uint32 conf_pct  = 4;
}

message LatencyClient {
  double prepare_p50 = 1;
  double prepare_p95 = 2;
  double wait_p50    = 3;
  double wait_p95    = 4;
  double parse_p50   = 5;
  double parse_p95   = 6;
  double e2e_p50     = 7;
  double e2e_p95     = 8;
  double overhead_vs_server_sum_p50 = 9;
  double overhead_vs_server_sum_p95 = 10;
}

message LatencyServer {
  double asr_p50     = 1;
  double asr_p95     = 2;
  double diar_p50    = 3;
  double diar_p95    = 4;
  double verify_p50  = 5;
  double verify_p95  = 6;
  double itn_p50     = 7;
  double itn_p95     = 8;
  double vad_p50     = 9;
  double vad_p95     = 10;
  double denoise_p50 = 11;
  double denoise_p95 = 12;
  double sum_p50     = 13;
  double sum_p95     = 14;
}

message LatencyUi {
  double on_chunk_total_p50 = 1;
  double on_chunk_total_p95 = 2;
}

message LatencySummary {
  LatencyClient client = 1;
  LatencyServer server = 2;
  LatencyUi     ui     = 3;
}

message SessionState {
  string              title              = 1;
  uint32              conf_threshold_pct = 2;
  repeated DisplayRow rows               = 3;
  repeated DisplayRow provisional_rows   = 4;
  repeated string     speaker_ids        = 5;
  repeated Highlight  highlights         = 6;
  uint32              n_phrases          = 7;
  uint32              n_low              = 8;
  repeated float      amp_trace          = 9  [packed = true];
  double              amp_trace_step_sec = 10;
  double              source_total_sec   = 11;
  double              source_seen_sec    = 12;
  double              speech_seen_sec    = 13;
  double              wall_elapsed_sec   = 14;
  double              playhead_ratio     = 15;
  bool                done               = 16;
  double              ts                 = 17;
  uint32              last_asr_chunk_ms  = 18;
  double              infer_p50_ms       = 19;
  double              infer_p95_ms       = 20;
  LatencySummary      latency            = 21;
}

message EventFlags {
  bool streaming  = 1;
  bool correction = 2;
  bool final      = 3;
}

message Diarization {
  repeated float flat_scores       = 1 [packed = true];
  repeated int32 shape             = 2 [packed = true];
  repeated int64 subframe_start_ms = 3 [packed = true];
  repeated int64 subframe_end_ms   = 4 [packed = true];
}

message CorrectionUpdate {
  string         text                = 1;
  string         full_text           = 2;
  string         committed_text      = 3;
  string         tail_text           = 4;
  repeated Word  merged_words        = 5;
  repeated int32 updated_indices     = 6 [packed = true];
  double         update_start_sec    = 7;
  double         update_end_sec      = 8;
  double         commit_boundary_sec = 9;
  uint32         num_committed       = 10;
  uint32         num_tail            = 11;
  double         itn_ms              = 12;
  double         merge_ms            = 13;
}

message Timing {
  double client_prepare_ms = 1;
  double client_wait_ms    = 2;
  double asr_ms            = 3;
  double diar_ms           = 4;
  double verify_ms         = 5;
  double itn_ms            = 6;
  double vad_ms            = 7;
  double denoise_ms        = 8;
}

message StartSessionResponse {
  string       session_id    = 1;
  int64        stream_id     = 2;
  uint64       state_version = 3;
  SessionState state         = 4;
}

message PushAudioResponse {
  string         session_id          = 1;
  int64          stream_id           = 2;
  uint64         state_version       = 3;
  EventFlags     events              = 4;
  double         source_seen_sec     = 5;
  double         speech_seen_sec     = 6;
  string         streaming_text      = 7;
  string         text                = 8;
  string         itn_text            = 9;
  string         itn_full_text       = 10;
  string         itn_correction_text = 11;
  repeated Word  asr_words           = 12;
  float          asr_confidence      = 13;
  repeated float asr_word_confidence = 14 [packed = true];
  string         speaker             = 15;
  float          speaker_prob        = 16;
  string         verified_name       = 17;
  float          verify_score        = 18;
  int64          chunk_start_ms      = 19;
  double         chunk_start_sec     = 20;
  double         chunk_end_sec       = 21;
  Diarization    diarization         = 22;
  CorrectionUpdate correction        = 23;
  Timing         timing              = 24;
}

message StateResponse {
  string       session_id          = 1;
  int64        stream_id           = 2;
  uint64       state_version       = 3;
  SessionState state               = 4;
  uint64       transcript_revision = 5;
  bool         transcript_final    = 6;
  double       commit_boundary_sec = 7;
}

message StopSessionResponse {
  string            session_id    = 1;
  int64             stream_id     = 2;
  uint64            state_version = 3;
  EventFlags        events        = 4;
  PushAudioResponse result        = 5;
  SessionState      state         = 6;
}

message TextEditRequest {
  string        session_id        = 1;
  uint64        base_revision     = 2;
  double        start_sec         = 3;
  double        end_sec           = 4;
  repeated Word replacement_words = 5;
  string        editor_id         = 6;
  string        note              = 7;
}

message CanonicalTranscript {
  uint64        revision            = 1;
  bool          final               = 2;
  double        commit_boundary_sec = 3;
  string        text                = 4;
  repeated Word words               = 5;
}

message ReviewEditResponse {
  string              session_id = 1;
  CanonicalTranscript transcript = 2;
  SessionState        state      = 3;
}

message ListSessionsRequest {
  uint32 limit  = 1;
  string cursor = 2;
}

message SessionSummary {
  string          session_id     = 1;
  string          title          = 2;
  double          created_at     = 3;
  double          updated_at     = 4;
  double          duration_sec   = 5;
  bool            final          = 6;
  bool            running        = 7;
  repeated string participants   = 8;
  string          security_level = 9;
  string          mode           = 10;
}

message ListSessionsResponse {
  repeated SessionSummary sessions    = 1;
  string                  next_cursor = 2;
}

message RenameSpeakerRequest {
  string session_id    = 1;
  string from_speaker  = 2;
  string to_speaker    = 3;
  string verified_name = 4;
  string editor_id     = 5;
  string note          = 6;
}

message PipelineTraceRequest {
  string          session_id = 1;
  uint64          after_seq  = 2;
  uint32          limit      = 3;
  repeated string stages     = 4;
}

message PipelineTraceEvent {
  uint64 seq             = 1;
  double ts              = 2;
  string stage           = 3;
  string event           = 4;
  double audio_start_sec = 5;
  double audio_end_sec   = 6;
  string payload_json    = 7;
}

message PipelineTraceResponse {
  string                      session_id = 1;
  repeated PipelineTraceEvent events     = 2;
  uint64                      next_seq   = 3;
  bool                        has_more   = 4;
  bool                        enabled    = 5;
  bool                        truncated  = 6;
  uint64                      max_bytes  = 7;
}

message AuditHistoryRequest {
  string session_id = 1;
  uint32 limit      = 2;
}

message AuditEvent {
  double ts           = 1;
  string event        = 2;
  string payload_json = 3;
}

message AuditHistoryResponse {
  string              session_id = 1;
  repeated AuditEvent events     = 2;
}

message ModelStatusRequest {}

message ModelStatusEntry {
  string name    = 1;
  string version = 2;
  string state   = 3;
}

message ModelStatusResponse {
  repeated ModelStatusEntry models = 1;
}
```

```protobuf
// speaker_registry.proto
syntax = "proto3";
package asr.ui.v1;

service SpeakerRegistryService {
  rpc GetEnrollmentScript     (GetEnrollmentScriptRequest)
      returns (GetEnrollmentScriptResponse);
  rpc EnrollSpeaker           (EnrollSpeakerRequest)
      returns (EnrollSpeakerResponse);
  rpc ListSessionSpeakers     (ListSessionSpeakersRequest)
      returns (ListSessionSpeakersResponse);
  rpc SaveSessionSpeakers     (SaveSessionSpeakersRequest)
      returns (SaveSessionSpeakersResponse);
  rpc GetSpeakerRegistryStatus(GetSpeakerRegistryStatusRequest)
      returns (GetSpeakerRegistryStatusResponse);
}

enum SpeakerDestination {
  SPEAKER_DESTINATION_UNSPECIFIED = 0;
  SESSION_ONLY                    = 1;
  GLOBAL_SHARED                   = 2;
}

message GetEnrollmentScriptRequest {}

message GetEnrollmentScriptResponse {
  string script_text              = 1;
  uint32 sample_rate              = 2;
  double recommended_duration_sec = 3;
  uint32 target_segments          = 4;
}

message EnrollSpeakerRequest {
  string display_name       = 1;
  bytes  wav                = 2;
  string editor_id          = 3;
  string note               = 4;
  bool   allow_below_policy = 5;
}

message EnrollSpeakerResponse {
  bool   ok                       = 1;
  string error                    = 2;
  string speaker_id               = 3;
  double raw_seconds              = 4;
  double speech_seconds_after_vad = 5;
  uint32 segments_enrolled        = 6;
  uint32 target_segments          = 7;
  string warning                  = 8;
  double db_mtime                 = 9;
}

message SessionSpeakerEvidence {
  double total_speech_sec      = 1;
  uint32 span_count            = 2;
  double staged_at             = 3;
  string source_verified_name  = 4;
}

message SessionSpeakerEntry {
  string                 session_speaker_id = 1;
  repeated string        diar_slots         = 2;
  string                 verified_name      = 3;
  double                 score              = 4;
  uint32                 windows            = 5;
  double                 created_at         = 6;
  double                 updated_at         = 7;
  string                 status             = 8;
  bool                   has_evidence       = 9;
  SessionSpeakerEvidence evidence           = 10;
  string                 published_name     = 11;
  double                 published_at       = 12;
  string                 publish_error      = 13;
}

message ListSessionSpeakersRequest  { string session_id = 1; }

message ListSessionSpeakersResponse {
  string                       session_id = 1;
  repeated SessionSpeakerEntry speakers   = 2;
}

message SpeakerSelection {
  string             session_speaker_id = 1;
  SpeakerDestination destination        = 2;
  string             global_name        = 3;
}

message SaveSessionSpeakersRequest {
  string                    session_id = 1;
  repeated SpeakerSelection selections = 2;
  string                    editor_id  = 3;
}

message SaveSpeakerResult {
  string session_speaker_id = 1;
  bool   ok                 = 2;
  string status             = 3;
  string error              = 4;
  uint32 segments_enrolled  = 5;
}

message SaveSessionSpeakersResponse {
  string                     session_id = 1;
  repeated SaveSpeakerResult results    = 2;
}

message SpeakerBelowPolicy {
  string spk_id            = 1;
  string spk_name          = 2;
  uint32 sample_count      = 3;
  double longest_sample_sec = 4;
  string reason            = 5;
  string kind              = 6;
}

message GetSpeakerRegistryStatusRequest { string session_id = 1; }

message GetSpeakerRegistryStatusResponse {
  double                      global_db_mtime        = 1;
  string                      global_db_revision     = 2;
  uint32                      global_speaker_count   = 3;
  bool                        sidecar_reachable      = 4;
  string                      session_id             = 5;
  uint32                      session_pending_count  = 6;
  uint32                      session_published_count = 7;
  uint32                      session_failed_count   = 8;
  repeated string             global_speaker_names   = 9;
  repeated SpeakerBelowPolicy speakers_below_policy  = 10;
}
```

---

## Phụ lục B — nơi đọc mã nguồn tham chiếu

| Cần biết | Đọc tệp |
|---|---|
| Đường method của từng RPC | `grpc/AsrClient.cpp` |
| Header, khung gRPC, mã lỗi | `grpc/GrpcChannel.cpp` |
| HTTP/2 và HPACK | `grpc/Http2Client.cpp`, `grpc/Hpack.cpp` |
| Số hiệu trường của mọi thông điệp | `proto/AsrSession.cpp`, `proto/SpeakerRegistry.cpp` |
| Bộ mã hoá/giải mã proto3 | `proto/ProtoWire.cpp` |
| Vòng đẩy audio, `seq`, chính sách thử lại | `core/SessionWorker.cpp` |
| Vòng poll trạng thái | `core/StatePoller.cpp` |
| Cách dựng `config_json` | `core/SessionWorker.cpp` — `buildConfigJson()` |
| Server giả để thử | `tools/mock_adapter.js` |
