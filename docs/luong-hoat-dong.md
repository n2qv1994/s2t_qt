# s2t_qt — Luồng hoạt động

Tài liệu này mô tả bên trong **hai chương trình**: các luồng (thread), các kênh
kết nối, vòng đời một phiên đi qua cả hai tiến trình, và lý do đằng sau những
chỗ trông có vẻ lạ. Dành cho người bảo trì mã nguồn.

| Tài liệu | Dành cho |
|---|---|
| [huong-dan-su-dung.md](huong-dan-su-dung.md) | người vận hành ứng dụng |
| **luong-hoat-dong.md** (tài liệu này) | người bảo trì mã nguồn |
| [danh-sach-api.md](danh-sach-api.md) | người tích hợp một client bên ngoài |

---

## 0. Hai tiến trình, và ranh giới giữa chúng

`s2t_qt` từng là một chương trình duy nhất nối thẳng lên adapter gRPC. Nó đã
được tách làm hai theo đúng chỗ mà `docs/slide.pdf` gọi là *Server buffer*
trong luồng tổng thể:

```
   máy trạm điều hành viên              máy đệm                   tầng suy luận
┌───────────────────────────┐   ┌────────────────────────┐   ┌──────────────────┐
│      s2t-qt-client        │   │     s2t-qt-server      │   │ Triton :8011     │
│  microphone, giao diện,   │──►│  hàng đợi audio,       │──►│  KServe v2       │
│  soát/sửa, đăng ký giọng  │   │  bản chép, bộ đệm      │   │  asr_diar_session│
│                           │◄──│                        │◄──│  ── hoặc ──      │
└───────────────────────────┘   └────────────────────────┘   │ Riva :50051      │
        gRPC :8800              InferenceBackend là ranh giới └──────────────────┘
   3 service, 20 RPC, 1 token
```

**Ranh giới cũ đã biến mất.** Cho tới 2026-08-25, bốn RPC do bộ đệm trả lời và
mười sáu RPC được chuyển tiếp nguyên vẹn tới một adapter Python sở hữu bản
chép, kho audio và cơ sở dữ liệu giọng nói. Adapter đó không còn trong sơ đồ
triển khai — người dùng cho biết nó vốn chỉ là mã mẫu để hiểu nghiệp vụ — nên
**không còn gì để chuyển tiếp**:

- Những RPC *về một cuộc họp đang chạy* được trả lời từ chính `SessionBuffer`
  giữ cuộc họp đó: `get_review_state`, `apply_text_edit`, `rename_speaker`,
  cộng với bốn RPC vốn đã có đệm.
- Những RPC *về một cuộc họp đã kết thúc* được trả lời từ `SessionStore`:
  `get_audio_range`, `get_audit_history`, `list_sessions`,
  `ListSessionSpeakers`, `SaveSessionSpeakers`. `get_review_state` cũng rơi
  về đây khi phiên không còn trong bộ nhớ — soát lại một cuộc họp tuần trước
  là trường hợp bình thường của RPC đó, không phải ngoại lệ.
- `get_model_status` là RPC duy nhất còn hỏi tầng suy luận từ một luồng kết
  nối, và nó chỉ hỏi danh sách mô hình.
- Ba RPC đăng ký giọng (`GetEnrollmentScript`, `EnrollSpeaker`,
  `GetSpeakerRegistryStatus`) đi qua **một dịch vụ HTTP khác hẳn**:
  `campp_native/enroll_service.py` trên `:8790`. Nó không phải tầng suy luận,
  và không thể là: `rebuild_db` cần quyền `docker exec` mà container Triton cố
  tình không có. Xem `s2t-qt-server/CampPlusClient.h`.
- `get_pipeline_trace` là RPC duy nhất còn là stub, và nó trả `enabled = false`
  — đúng cách hợp đồng vốn đã dành để nói "bản triển khai này không thu thập
  trace", nên client không phải xử lý lỗi.

Khi `database/dir` hoặc `enroll/url` để trống, những RPC phụ thuộc chúng trả
`FAILED_PRECONDITION` kèm một câu điều hành viên hiểu được, chứ **không** trả
về thành công rỗng: một bản chép trống trông như câu trả lời thật là kiểu hỏng
tệ hơn nhiều so với một mã lỗi rõ ràng.

**Ranh giới mới là `s2t-qt-server/backend/InferenceBackend.h`.** Dưới nó có
`TritonBackend` (KServe v2, kho mô hình đang chạy) và `RivaBackend`
(`nvidia.riva.asr`, luồng bidi). Không có gì phía trên biết mình đang nói với
bên nào; `upstream/backend` trong tệp cấu hình chọn.

**Hệ quả lớn nhất: bản chép được dựng ở đây.** Riva và Triton đều trả lời theo
từng gói và không nhớ gì cả, nên `asr::SessionState` — rows, phrases, làn
người nói, dải biên độ — được bồi đắp trong `LiveTranscript`. `get_live_state`
không còn là bộ nhớ đệm của câu trả lời từ trên mà là một khoá và một bản sao.

**Cái gì đổi chỗ.** Vòng đẩy audio, tính bất biến theo `seq` và quy tắc chỉ thử
lại lỗi vận chuyển trước đây nằm trong `SessionWorker` của client. Chúng vẫn
còn nguyên ở đó — nhưng giờ chỉ chạy trên chặng *client → bộ đệm*, và một bản
sao của cùng những quy tắc ấy chạy trên chặng *bộ đệm → tầng suy luận* bên
trong `SessionBuffer`. Kết quả: một sự cố mạng ở máy trạm tốn độ sâu hàng đợi
chứ không tốn tiếng nói.

**Cái gì không đổi.** Số hiệu trường proto3, quy tắc `INTERNAL` không bao giờ
được thử lại, ý nghĩa của `ACK`, tính ba trạng thái của `expected_speakers`, và
rào chắn drain trước `stop_session`. Xem mục 14.

---

## 1. Toàn cảnh — `s2t-qt-client`

```
                        ┌──────────────── luồng GUI ────────────────┐
                        │  MainWindow   TimelineView   ReviewPanel  │
                        │  Dialogs   TraceWindow   EvidenceWindow   │
                        │  DiagnosticsWindow                        │
                        └───────────────────┬───────────────────────┘
                                            │  chỉ nói chuyện với
                                    ┌───────▼────────┐
                                    │SessionController│  (mặt tiền, luồng GUI)
                                    └───┬───┬────┬───┘
                 ┌──────────────────────┘   │    └──────────────────┐
                 │                          │                       │
        ┌────────▼────────┐   ┌─────────────▼──────┐   ┌────────────▼─────────┐
        │ audio-capture   │   │  session-worker    │   │    state-poller      │
        │ QAudioSource    │   │  QThread + kênh    │   │  QThread + kênh riêng │
        │ ~20 ms/lần      │   │  riêng             │   │  get_live_state 200ms │
        └────────┬────────┘   └─────────▲──────────┘   └───────────┬──────────┘
                 │  AudioQueue           │                         │
                 └───────────────────────┘                         │
                                                                   │
        ┌──────────────────── RpcExecutor: 3 lane ─────────────────┴───────────┐
        │ rpc-lane-0        rpc-lane-1        rpc-lane-2                       │
        │ mỗi lane một AsrClient = một kết nối TCP riêng                       │
        └──────────────────────────────┬───────────────────────────────────────┘
                                       │
                        ┌──────────────▼──────────────┐
                        │ AsrClient  →  grpc::Channel │
                        │ ProtoWire · HPACK · HTTP/2  │  (tự viết, không thư viện)
                        └──────────────┬──────────────┘
                                       │  h2c, bearer token
                                  Server buffer :8800
```

### Vì sao nhiều kênh như vậy

Một `grpc::Channel` ứng với đúng một kết nối TCP và thuộc về đúng một luồng.
Việc đẩy audio, việc poll trạng thái và các RPC theo yêu cầu **không dùng chung
kênh**:

- `get_live_state` phình theo độ dài cuộc họp. Nếu nó dùng chung kênh với audio
  thì một lần tuần tự hoá trạng thái lớn sẽ nằm chắn trước một gói audio 160 ms.
- Một `EnrollSpeaker` được phép chạy tới 120 giây. Ba lane là con số nhỏ nhất
  giữ cho nó không chắn cả bộ chọn phiên lẫn lần poll trạng thái model phía sau.

Đây cũng chính là hình dạng mà cầu nối Python trước đây dùng — và, sau khi
tách, chính là hình dạng mà `s2t-qt-server` dựng lại ở phía nó.

---

## 1b. Toàn cảnh — `s2t-qt-server`

```
                     ┌──────────── luồng main (có vòng lặp sự kiện) ───────────┐
                     │  QTcpServer nhận kết nối · BufferHub dọn phiên hết hạn  │
                     └───────────────────────┬─────────────────────────────────┘
                                             │  mỗi kết nối một luồng
      ┌──────────────────┬───────────────────┴──────────────┬──────────────────┐
 ┌────▼─────┐      ┌─────▼──────┐                     ┌─────▼──────┐     ┌─────▼─────┐
 │http2-    │      │http2-      │        ...          │http2-      │     │http2-     │
 │conn-1    │      │conn-2      │                     │conn-N      │     │conn-…     │
 └────┬─────┘      └─────┬──────┘                     └─────┬──────┘     └─────┬─────┘
      └──────────────────┴──── GrpcServer::dispatch ────────┴──────────────────┘
                                       │
              ┌────────────────────────┼─────────────────────────┐
              │                        │                         │
     ┌────────▼─────────┐   ┌──────────▼──────────┐   ┌──────────▼──────────┐
     │  BufferService   │   │   BufferService     │   │   BufferService     │
     │ 7 RPC phiên sống │   │ 5 RPC phiên đã lưu  │   │   3 RPC quản trị    │
     │ + 1 stub (trace) │   │ + 3 RPC đăng ký giọng│  │                     │
     └────────┬─────────┘   └──────────┬──────────┘   └─────────────────────┘
              │                        │
              │            ┌───────────┴───────────┬─────────────────┐
              │            │                       │                 │
   ┌──────────▼───────────────────────┐  ┌─────────▼────────┐  ┌─────▼──────────┐
   │ SessionBuffer (mỗi phiên một cái)│  │  SessionStore    │  │ CampPlusClient │
   │  ├ forward-<phiên>               │  │  SQLite + tệp    │  │  HTTP/1.1      │
   │  │   hàng đợi → BackendSession   │  │  .s16le mỗi họp  │  │                │
   │  └ LiveTranscript                │  └──────────────────┘  └─────┬──────────┘
   │      bản chép, dưới m_stateMutex │      ┌──────────────────┐    │
   └──────────┬───────────────────────┘      │ upstream-probe   │    │ enroll_service.py
              │                              │ ping mỗi 5 giây  │    └──► :8790
              │                              └────────┬─────────┘
   ┌──────────▼───────────────────────┐               │
   │ InferenceBackend                 │◄──────────────┘
   │  TritonBackend  │  RivaBackend   │   qua kênh quản trị
   │  ModelInfer     │  Streaming-    │   (triton-admin / riva-admin)
   │  (unary/gói)    │  Recognize     │
   └──────────┬───────────────────────┘
              │  h2c, bearer token của tầng suy luận
        Triton :8011  hoặc  Riva :50051
```

Ba nhánh, ba nơi lưu, và ranh giới giữa chúng là điều đáng nhớ nhất ở sơ đồ
này: **hàng đợi** (`SessionJournal`, xoá khi tầng suy luận đã nhận), **bản lưu**
(`SessionStore`, giữ lại cuộc họp), và **cơ sở dữ liệu giọng** (CAM++, không do
máy chủ này quản lý).

### Vì sao mỗi kênh upstream lại có một luồng riêng

Đây là một quy tắc của Qt dễ quên: **`QTcpSocket` thuộc về luồng đã tạo nó.**
`grpc::Channel` mở socket một cách lười, ngay bên trong lời gọi đầu tiên — nên
một kênh dùng chung giữa hai luồng kết nối HTTP/2 sẽ có socket được tạo ở luồng
này và đọc ở luồng kia. Nó chạy được đủ thường xuyên để qua mắt một phép thử
sơ sài, và vẫn là hành vi không xác định.

Vì vậy kênh không bao giờ rời khỏi luồng của nó. `RpcLane` là lớp gói cho điều
đó: người gọi đưa việc *vào* luồng ấy bằng `Qt::BlockingQueuedConnection` rồi
chờ. Chờ là đúng — người gọi là một luồng kết nối HTTP/2 đang nợ một câu trả
lời trên chính lời gọi đó.

`SessionBuffer` tạo `AsrClient` của vòng đẩy ngay bên trong `run()` của chính
nó, nên nửa ấy không cần `RpcLane`.

---

## 2. Các luồng

### `s2t-qt-client`

| Tên luồng | Tạo bởi | Việc |
|---|---|---|
| `main` | Qt | Toàn bộ widget. Không bao giờ chặn ở I/O mạng. |
| `audio-capture` | `SessionController` | `QAudioSource` + `mic::DenoiseAbRecorder`. Sống suốt vòng đời controller. |
| `session-worker` | `SessionController` | Một phiên trọn vẹn: `start_session` → vòng đẩy audio → `stop_session`. |
| `state-poller` | `SessionController` | `get_live_state` mỗi 200 ms. Sống suốt vòng đời controller; đổi phiên không tốn một lần kết nối lại. |
| `rpc-lane-0..2` | `RpcExecutor` | Các RPC theo yêu cầu: review, sửa, audio, đăng ký giọng, trace. |
| `diagnostics` | `DiagnosticsWindow` | Tạm thời, chỉ chạy trong lúc thực hiện một phép chẩn đoán. Mỗi lúc chỉ một cái: bộ thu báo cáo của `SelfTest` là biến toàn cục. |

### `s2t-qt-server`

| Tên luồng | Tạo bởi | Việc |
|---|---|---|
| `main` | Qt | `QTcpServer` nhận kết nối, `BufferHub` dọn phiên đã hết hạn mỗi 5 giây, và bộ đếm tín hiệu 200 ms. Không bao giờ chặn ở I/O mạng. |
| `http2-conn-N` | `Http2Server` | Một kết nối TCP: đọc khung, giải mã HPACK, gọi handler, ghi trả lời. Chặn có chủ ý. Có giới hạn số lượng (mặc định 128); quá thì kết nối mới bị từ chối bằng GOAWAY chứ không bị nhận rồi bỏ đói. |
| `forward-<phiên>` | `SessionBuffer` | Rút hàng đợi của một phiên, `push_audio` lên tầng suy luận, rồi cuối cùng `stop_session`. Một kênh riêng. |
| `state-<phiên>` | `SessionBuffer` (qua `RpcLane`) | Làm mới bộ đệm `get_live_state` của phiên đó. Một kênh riêng. |
| `upstream-probe` | `BufferHub` | `ping` tầng suy luận mỗi 5 giây, và ngay sau khi một lời gọi thất bại. Luồng riêng, vì một phép thử chạy trên luồng nhận kết nối sẽ chặn vòng accept đúng bằng cả timeout của tầng suy luận. |
| `triton-admin` / `riva-admin` | backend | Kênh quản trị duy nhất còn lại: `ping` và danh sách mô hình. `RpcLane` giữ nó trên một luồng riêng vì `QTcpSocket` thuộc về luồng đã tạo nó. |

Số luồng của server tăng theo **số điều hành viên và số cuộc họp**, không theo
số request: mọi RPC ở đây đều là unary, và một luồng chỉ giữ socket trong thời
gian một cặp request/response.

Mọi thứ đi qua ranh giới luồng bằng signal/`invokeMethod` dạng queued đều phải
là metatype đã đăng ký, nếu không Qt lặng lẽ bỏ lời gọi lúc chạy (xem
`s2t-qt-client/main.cpp`).

**Ngoại lệ có chủ ý:** tín hiệu `AudioCapture::chunk` nối bằng
`Qt::DirectConnection`. Nó chạy trên luồng capture và chỉ chạm vào `AudioQueue`
vốn đã có khoá riêng, nên audio không phải đi vòng qua vòng lặp sự kiện của GUI
để tới được bên gửi.

**Thứ tự khoá trong `SessionBuffer`:** `m_stateMutex` trước `m_mutex`, không
bao giờ ngược lại. `snapshot()` cần cả hai, và nó là chỗ duy nhất dễ mắc lỗi.

---
## 3. Vòng đời một phiên microphone

```
người dùng bấm BẮT ĐẦU GHI ÂM
   │
   ├─ StartSessionDialog: siêu dữ liệu + phạm vi người nói
   │
   ▼
SessionController::startMicrophone()
   ├─ cấu hình AudioQueue = bufferSec × bytes/giây
   ├─ giữ lại SessionWorker::Settings (m_pendingSettings)
   └─ invokeMethod(capture, "start")            ──► luồng audio-capture
                                                      │
                                                      ├─ chọn thiết bị (id + tên)
                                                      ├─ kiểm tra định dạng
                                                      └─ QAudioSource::start()
                                                      │
   ┌──────────────────────────────────────────────────┘
   ▼
onCaptureStarted()   ← chỉ tới đây phiên mới được tạo trên server
   └─ beginSession(new SessionWorker)           ──► luồng session-worker
                                                      │
                                                      ├─ start_session (deadline 20 s)
                                                      ├─ emit sessionStarted(id)
                                                      │     └─► poller->setSession(id)
                                                      └─ runMicrophone()
```

> **Vì sao mở thiết bị trước rồi mới tạo phiên.** Một lỗi driver phải đọc thành
> "không ghi được", chứ không phải để lại một cuộc họp rỗng mở trên server mà
> không bao giờ có gì tới. Vì vậy worker chưa được khởi động cho tới khi
> `onCaptureStarted` xác nhận có thu thật.

### Vòng đẩy audio (`runMicrophone`)

```
lặp cho tới khi có yêu cầu dừng:
    nếu có chunk bị mất  → dừng phiên, báo lỗi   (không im lặng mất tiếng)
    nếu đang tạm dừng    → xoá gói dở, ngủ 30 ms
    lấy dữ liệu từ AudioQueue (chờ tối đa 100 ms)
    gom đủ 160 ms → pushPacket()
sau vòng lặp:
    xả nốt hàng đợi (hữu hạn: capture đã được lệnh dừng trước)
    drainAndStop()
```

Gói truyền tải là **160 ms** cho microphone. Capture giao dữ liệu ~20 ms một
lần; việc gom lại thành 160 ms có nghĩa là không có công việc mạng hay protobuf
nào nằm trên đường đi của audio.

Phát lại tệp dùng **320 ms** — đúng bước nhảy diar tự nhiên của pipeline; nó
giảm một nửa chi phí request và poll so với 160 ms mà không vượt giới hạn nạp
của model.

### Kết thúc (`drainAndStop`)

1. Đẩy phần đuôi còn lại (đã cắt cho tròn frame).
2. `stop_session` — **deadline 3600 giây**. Đây là rào chắn xả: sau một lần
   upload tệp nhanh hoặc sau khi phục hồi mạng, hàng đợi phía server có thể còn
   giữ hàng phút audio. Deadline ngắn sẽ chốt nhầm một phiên chưa xử lý xong.
3. `get_review_state` để lấy số revision cuối. Thất bại ở bước này **không** làm
   hỏng phiên — cuộc họp đã dừng và đã lưu, chỉ là bản tóm tắt thiếu số revision.
4. `emit finished(summary)`.

### Vì sao `stop_session` phải cùng luồng với vòng đẩy

`stop_session` là một rào chắn xả: nó bắt buộc phải đến **sau** gói audio cuối
cùng đã được gửi. Cách duy nhất bảo đảm thứ tự đó là giữ cả hai trên cùng một
luồng, thay vì đi phối hợp hai luồng với nhau.

---

## 4. Luồng phát lại tệp

Khác luồng microphone ở hai điểm, còn lại dùng chung mã:

```
startFile(path)
   ├─ audio::ensurePcmWav()   .m4a / .wav lạ → ffmpeg → WAV PCM tạm
   ├─ wav::readWav()          nạp toàn bộ vào bộ nhớ, xoá tệp tạm ngay
   └─ beginSession(new SessionWorker)   ← thẳng luôn, không có capture
```

Không có `AudioQueue` (worker nhận `nullptr`). Nếu bật *pacedToSourceClock*,
vòng lặp tự ngủ để bám theo đồng hồ của nguồn; tắt thì đẩy nhanh hết mức
pipeline nhận.

Worker lấy sample rate và số kênh **từ chính tệp**, không lấy từ cấu hình
microphone: `push_audio` khai báo với server đúng cái đang gửi, sai chỗ này sẽ
làm hỏng mọi mốc thời gian mà pipeline suy ra từ số mẫu.

---

## 5. AudioQueue — bị chặn có chủ ý

```
capture (20 ms/lần) ──push──► [ bộ đệm có trần ] ──take──► worker (160 ms/gói)
```

- **Có trần**: khi server không theo kịp, mục tiêu là **dừng ầm ĩ** chứ không
  âm thầm xoá audio. `push()` trả `false` khi đầy; controller ghi log ERROR và
  gọi `worker->noteDroppedChunk()`, worker dừng phiên ở vòng lặp kế tiếp.
- **Tạm dừng bỏ dữ liệu ở lúc push, không phải lúc gửi**: lời nói trong lúc tạm
  dừng không bao giờ được giao sau đó như thể nó là trực tiếp. Gói đang gom dở
  cũng bị xoá, để một chunk vừa kịp chen trước cú bấm không được gửi sau khi
  tiếp tục.

---

## 6. Xử lý sự cố khi đang chạy

### Mất mạng

`pushPacket` phân loại status trả về:

| Nhóm | Mã | Xử lý |
|---|---|---|
| Truyền tải | `UNAVAILABLE`, `DEADLINE_EXCEEDED`, `CANCELLED` | Gửi lại **đúng seq đó**. An toàn: adapter phát lại phản hồi đã lưu. |
| Còn lại | `INTERNAL`, ... | **Không** thử lại, kết thúc phiên bằng lỗi. |

> `INTERNAL` bị loại khỏi nhóm thử lại một cách có chủ ý: adapter trả nó đúng
> vào lúc server *có thể đã* nạp phần audio này rồi, nên thử lại mù sẽ nhân đôi
> chữ trong bản chép.

Khi gặp lỗi truyền tải, client **đóng hẳn kết nối** (`client.reset()`) rồi ngủ
500 ms trước khi thử lại, thay vì ngồi chờ cơ chế kết nối lại của HTTP/2 trên
một socket đã chết — một kết nối mới sẽ thấy ngay mạng đã hồi phục.

Trong lúc đó trạng thái là `network_reconnecting`, phiên vẫn sống, audio mới
dồn vào `AudioQueue`.

### Mất microphone

`AudioCapture::checkHealth` chạy mỗi 2 giây và phát hiện ba dạng hỏng:

1. `QAudioSource` báo mã lỗi.
2. Thiết bị không còn trong danh sách của hệ điều hành.
3. Thiết bị còn đó nhưng **không có byte mới** giữa hai nhịp kiểm tra — một
   endpoint cũ có thể sống lâu hơn phần cứng và giữ luồng ở trạng thái "đang
   chạy" trong khi không giao gì cả.

Cộng thêm ràng buộc **tên thiết bị**: sau khi rút USB mic, hệ điều hành có thể
tái dùng endpoint cho thiết bị khác, và không có ràng buộc này thì ứng dụng sẽ
thu nhầm mà không báo.

Khi mất thiết bị: **không chèn khoảng lặng, không kết thúc phiên**. Hàng đợi
được xoá, trạng thái chuyển `device_reconnecting`, và timer thử mở lại 500 ms
một lần. Audio trong lúc mất thiết bị không lấy lại được vì nó chưa từng được
thu, nhưng phần trước và sau vẫn thuộc cùng một cuộc họp.

### Máy trạng thái mic

```
                    ┌──────────────── error ◄──── (lỗi không cứu được)
                    │
idle ──► starting ──┴──► recording ◄────► paused
                            │  ▲
                            │  └──── network_reconnecting
                            │  └──── device_reconnecting
                            ▼
                        finalizing ──► stopped
```

Khoá `micStatusKey()`: `starting`, `recording`, `paused`,
`network_reconnecting`, `device_reconnecting`, `finalizing`, `stopped`, `error`.

Một chuyển trạng thái lặp lại chính nó bị bỏ qua — nó không được phép khởi động
lại đồng hồ hay làm ngập log. `device_reconnecting` **thắng** `recording` do
worker báo: worker chỉ biết là không có audio tới, chứ không biết vì sao.

---

## 7. Đường đọc: state-poller

```
mỗi 200 ms, nếu có session_id:
    get_live_state (deadline 20 s)
    ├─ thành công → emit stateReceived(state, pollMs)
    └─ thất bại   → nếu là lỗi truyền tải thì reset kênh; emit pollFailed(...)
```

Poll hỏng **không bao giờ là lỗi chí mạng**: đường audio có kênh riêng và luật
thử lại riêng, một phiên không được phép kết thúc chỉ vì một truy vấn phía đọc
bị quá hạn.

`session_id` rỗng làm vòng lặp đứng chờ mà không phá luồng, nên chuyển cuộc họp
không tốn một lần kết nối lại.

Controller nhận `stateReceived` và:
- Cập nhật telemetry độ trễ poll.
- Tính lại hàng đợi phía server: `sentSec - state.sourceSeenSec`. ACK nghĩa là
  "đã lưu bền", **không** phải "đã suy luận xong"; tính lại ở đây giữ cho con số
  còn giảm được sau một đợt upload dồn khi không còn ACK nào để tính từ đó.
- Nếu đang soát lại một cuộc họp *khác*, bỏ qua — không đè phần đuôi trực tiếp
  lên nó.

---

## 7b. Bên trong Server buffer

### 7b.1 Vòng đời một phiên, phía server

```
client                     BufferService              SessionBuffer         tầng suy luận
  │  start_session             │                           │                      │
  ├───────────────────────────►│  tự sinh session_id       │                      │
  │                            │  tạo SessionBuffer ──────►│ (1 luồng forwarder)  │
  │                            │                           ├─ mở BackendSession ─►│
  │                            │◄── openBackend() chờ ─────┤                      │
  │◄─────── session_id ────────┤                           │                      │
  │                            │                           │
  │  push_audio seq=1          │                           │
  ├───────────────────────────►├──── enqueue ─────────────►│
  │◄─── ACK ngay lập tức ──────┤                           ├──── push_audio ──────►│
  │  ... 160 ms/gói ...        │                           │  (theo nhịp của nó)
  │                            │                           │
  │  get_live_state            │                           │
  ├───────────────────────────►├──────────────────────────►│  bộ đệm còn mới? trả luôn
  │◄─── trạng thái ────────────┤                           ├──── nếu quá 200 ms ──►│
  │                            │                           │
  │  stop_session              │                           │
  ├───────────────────────────►├──────────────────────────►│  đặt cờ dừng, đánh thức
  │                            │                           ├─ đẩy nốt gói còn lại ►│
  │                            │                           ├──── stop_session ────►│
  │◄─── trạng thái cuối ───────┤◄──────────────────────────┤◄─────────────────────┤
```

Điểm mấu chốt: **ACK của `push_audio` được trả ngay khi gói nằm trong hàng
đợi**, chứ không đợi tầng suy luận. Đó là toàn bộ giá trị của bộ đệm. Nhưng nó
không làm hỏng ý nghĩa của các con số mà điều hành viên đọc, vì:

- `sourceSeenSec` trong ACK vẫn là **con số của tầng suy luận**, chép từ phản
  hồi `push_audio` gần nhất mà nó thật sự trả lời. Client lấy
  `đã gửi − sourceSeenSec` để ra "hàng đợi server AI", nên con số đó giờ tính
  cả phần đang nằm trong bộ đệm. Đúng hơn trước, không phải kém đi.
- `timing.clientPrepareMs` / `clientWaitMs` cũng được chuyển tiếp nguyên, nên
  ước lượng thời gian truyền tải của client vẫn trừ đi công việc thật của tầng
  suy luận thay vì tính nó thành thời gian mạng.

### 7b.2 Hàng đợi có giới hạn

Mỗi phiên có một hàng đợi trong RAM, mặc định 300 giây audio (`buffer/seconds`)
— khoảng 28 MB ở 48 kHz mono 16-bit. Khi đầy, `push_audio` trả
`RESOURCE_EXHAUSTED` kèm thông báo nói rõ tầng suy luận đang không theo kịp.

Từ chối là **có chủ ý**. Một hàng đợi không giới hạn biến một tầng suy luận
đình trệ thành một cú kill vì hết bộ nhớ, và client vốn đã được viết để dừng ồn
ào khi bị từ chối chứ không phải để đi tiếp với một lỗ trong audio.

`buffer/spool_dir` **không** phải là chỗ hàng đợi tràn vào. Đó là bản sao để
đối chiếu: mọi gói đã nhận cũng được ghi nối vào `<phiên>.pcm`, để audio mà
tầng suy luận đã được đưa vẫn còn tồn tại sau sự việc. Bật hay tắt nó không đổi
gì ở hành vi khi đầy bộ đệm.

### 7b.3 Phát lại ACK cho `seq` gửi lại

Nếu `seq` tới nhỏ hơn hoặc bằng `seq` lớn nhất đã nhận, bộ đệm **phát lại ACK
đã lưu** và không xếp hàng gì thêm. Đây đúng là hành vi của adapter, và nó là
thứ duy nhất làm cho phép thử lại sau `DEADLINE_EXCEEDED` của client an toàn
trên một đường ống có trạng thái.

Bài kiểm tra `--selftest` khẳng định điều này bằng cách gửi lại gói cuối rồi
đếm số gói mà tầng suy luận giả thật sự nhận được.

### 7b.4 Bộ đệm trạng thái

`get_live_state` được phục vụ từ bộ nhớ đệm nếu nó mới hơn `buffer/state_poll_ms`
(mặc định 200 ms). Mười client cùng theo dõi một cuộc họp vì thế chỉ tốn của
tầng suy luận **một** lần đọc.

Bộ đệm được gieo sẵn bằng chính trạng thái mà `start_session` trả về, nên lần
poll đầu tiên ngay sau khi bắt đầu — lúc client hỏi dồn nhất — không tốn một
vòng đi về nào.

Khi làm mới thất bại: nếu bộ đệm còn dưới **2 giây tuổi**, nó vẫn được trả về.
Client poll năm lần một giây; để một lần poll hỏng làm nhấp nháy cả khung nhìn
bản chép thì tệ hơn là hiện nó chậm 200 ms. Quá 2 giây thì trạng thái lỗi được
trả thẳng cho client — che giấu lâu hơn thế là nói dối.

### 7b.5 Khi tầng suy luận sập

Vòng đẩy giữ nguyên gói và thử lại **cùng một `seq`**, với `reset()` kênh giữa
các lần và nghỉ 500 ms — đúng như client vẫn làm. Trong lúc đó:

- `push_audio` từ client vẫn được nhận và ACK bình thường; hàng đợi lớn dần.
- `upstream-probe` chuyển sang "không tới được", và `ping` của bộ đệm báo
  `upstream_ready = false`, nên đèn trên client chuyển **vàng — đang đệm**, chứ
  không đỏ.
- Cuộc họp chỉ đổ vỡ khi hàng đợi đầy, và lúc đó nó đổ vỡ ồn ào.

Với một lỗi **không** phải lỗi vận chuyển (`INTERNAL` chẳng hạn), gói không bao
giờ được thử lại: bộ đệm ghi nhận trạng thái đó là chí mạng cho phiên, dọn hàng
đợi, và trả **chính mã lỗi ấy** cho lần `push_audio` tiếp theo của client. Quy
tắc "không bao giờ thử lại `INTERNAL`" nhờ vậy vẫn kết luận đúng ở phía client,
y như khi nó nói chuyện thẳng với adapter.

### 7b.6 Rào chắn drain

`stop_session` từ client không được chuyển tiếp ngay. Nó đặt cờ dừng, đánh thức
vòng đẩy, rồi **chờ**. Vòng đẩy tiếp tục rút hàng đợi cho tới khi rỗng, và chỉ
khi đó mới gọi `stop_session` lên tầng suy luận — **trên cùng luồng đã gửi
audio**, đó là cách duy nhất bảo đảm thứ tự.

Nếu drain chưa xong trong deadline của client, bộ đệm trả `DEADLINE_EXCEEDED`
kèm số gói còn lại và vẫn tiếp tục drain. Lần `stop_session` sau đó nhận lại
đúng câu trả lời đã lưu, không phải một câu trả lời thứ hai khác.

### 7b.7 Phiên sống sót qua lần khởi động lại

Mặc định hàng đợi chỉ nằm trong RAM, và khi đó khởi động lại là mất mọi cuộc
họp đang mở: `push_audio` cho phiên cũ trả `NOT_FOUND` với thông báo nói rõ
nguyên nhân là nhật ký đang tắt. Đặt `buffer/journal_dir` thì khác hẳn.

**Hình dạng trên đĩa** (`SessionJournal.h`): mỗi phiên một nhật ký nối-thêm,
chia thành phân đoạn.

```
<dir>/<handle>.000001.jrn
<dir>/<handle>.000002.jrn

  đầu phân đoạn  "S2TJ" | version u32 | segment u32 | dự trữ u32
  bản ghi        type u8 | length u32 | payload | crc32 u32
```

Bốn loại bản ghi: `Meta` (một lần ở đầu mỗi phân đoạn), `Packet`, `Progress`,
`Stopped`. Payload mã hoá bằng chính bộ mã proto3 của dự án — số hiệu trường ở
đây là nội bộ, không ai ngoài tiến trình này đọc, nên chúng đổi được cùng với
số phiên bản trong đầu phân đoạn.

`handle` **không phải** `session_id`: mã phiên do tầng suy luận cấp nên không
được tin là an toàn cho tên tệp. Ký tự lạ bị thay, và một digest ngắn của mã
gốc được nối vào — vừa giữ hai mã khác nhau không đụng tên, vừa làm ánh xạ đó
tái lập được sau mỗi lần khởi động.

**Hai quy tắc về thứ tự, và toàn bộ tính đúng đắn nằm ở đó:**

1. **`Packet` được ghi TRƯỚC khi client được ACK.** ACK nghĩa là "đã nằm chắc
   trong bộ đệm"; điều đó phải còn đúng sau khi khởi động lại, nếu không thì nó
   chưa bao giờ đúng. Ghi hỏng là **từ chối gói** (`INTERNAL`), không phải một
   dòng cảnh báo — một phiên âm thầm hạ cấp xuống "không bền" còn tệ hơn một
   phiên từ chối thẳng.
2. **`Progress` được ghi SAU khi đẩy lên tầng suy luận thành công.** Chết giữa
   hai việc đó thì `seq` ấy được gửi lại ở lần khởi động sau, và tính bất biến
   theo `seq` của tầng suy luận biến bản trùng thành vô hại. Ghi ngược lại thì
   mất hẳn gói — bản ghi nói "đã gửi" trong khi chưa.

**Bản ghi rách ở cuối là bình thường, không phải hỏng.** Một cú chết giữa lúc
ghi để lại đúng một bản ghi dở. CRC bắt được, và bộ đọc coi đó là hết dữ liệu.
Đây là hình dạng thường gặp nhất của một nhật ký sau đúng cái sự cố mà tính
năng này sinh ra để lo, nên nó phải đọc được, không phải báo lỗi.

**Khôi phục** chạy trong constructor của `BufferHub`, trước khi cổng được mở —
một client kết nối lại ngay giây đầu tiên phải thấy cuộc họp đã ở đó. Với mỗi
nhật ký: đọc `Meta`, lấy `Progress` cao nhất làm mốc, nạp mọi `Packet` có `seq`
lớn hơn mốc vào thẳng hàng đợi. Vòng đẩy vì thế gửi phần tồn đọng **trước** bất
cứ thứ gì client đẩy tiếp — thứ tự xuyên qua lần khởi động lại được giữ nguyên.

Có `Stopped` nghĩa là cuộc họp đã kết thúc sạch sẽ: nhật ký bị xoá, phiên không
được dựng lại. Bản ghi đó luôn được `fsync`, bất kể chế độ, vì dựng lại một
phiên đã xong còn tệ hơn cái giá của một lần đồng bộ.

**Với client thì một lần khởi động lại là vô hình.** Nó nhận vài lỗi vận
chuyển, vòng thử lại sẵn có gửi lại đúng `seq` cũ, bộ đệm phát lại ACK đã lưu,
và cuộc họp đi tiếp. Hàng đợi 60 giây mặc định của client hấp thụ trọn một lần
khởi động lại thông thường.

**Độ bền là một lựa chọn, và tài liệu nói thật về nó:**

| `buffer/durability` | Chịu được | Giá |
|---|---|---|
| `os` (mặc định) | tiến trình chết, máy chủ khởi động lại | không đáng kể |
| `fsync` | thêm cả mất điện đột ngột | một `fsync` mỗi gói |

Ngay cả ở chế độ `os`, `Journal::write()` vẫn gọi `QFile::flush()` sau mỗi bản
ghi. Không có nó thì bản ghi "bền" vẫn còn nằm trong tiến trình lúc nó chết —
đúng cái tình huống duy nhất mà cả tệp này sinh ra để chống.

**Dung lượng đĩa** bị chặn bởi chính cái trần đã chặn hàng đợi. Với
`journal_keep=queue` (mặc định), một phân đoạn bị xoá ngay khi tầng suy luận đã
nhận hết mọi thứ trong đó, nên đĩa bám theo độ sâu hàng đợi chứ không theo độ
dài cuộc họp. `journal_keep=session` giữ lại tất cả và biến nhật ký thành bản
lưu của cả cuộc họp, đổi lại ~96 kB mỗi giây.

**Những gì nhật ký không làm.** Nó không làm bộ đệm chứa được nhiều hơn — đầy
vẫn là `RESOURCE_EXHAUSTED`. Và nó không cứu được một phiên mà **tầng suy luận**
đã bỏ: nếu adapter quên phiên trong lúc bộ đệm nằm xuống, gói đầu tiên gửi lại
nhận một lỗi không phải lỗi vận chuyển, và phiên kết thúc ồn ào đúng như mọi
lỗi chí mạng khác.

Chuyển tiếp mù `push_audio` cho một phiên không có bộ đệm sẽ làm mọi bộ đếm nói
dối, nên nó vẫn không được phép — kể cả khi tầng suy luận vẫn còn giữ phiên ấy.
Bản chép cũ thì vẫn soát lại được qua `get_review_state`, vì đó là RPC chuyển
tiếp và không cần bộ đệm.

### 7b.7b Phiên mồ côi

Một phiên được khôi phục mà không client nào quay lại nhận sẽ bị đóng sau
`buffer/orphan_timeout_sec` (mặc định 1800 giây) — phần tồn đọng vẫn được đẩy
lên trước, vì việc đó xảy ra ngay khi vòng đẩy chạy. Không có mốc này thì một
máy chủ khởi động lại vài lần sẽ tích lại những cuộc họp không bao giờ kết thúc.

Chỉ phiên **được khôi phục** mới bị tính là mồ côi. Một client đang sống mà im
lặng một lúc là chuyện khác và được để yên.

Bộ dọn chạy trên luồng `main` — luồng nhận kết nối — nên nó gọi `requestStop()`
chứ không phải `stop()`: đặt cờ rồi đi tiếp. Vòng đẩy làm phần drain và
`stop_session` trên luồng của chính nó, và nhịp dọn sau thấy phiên đã kết thúc.

### 7b.8 Dọn dẹp và tắt

- Phiên đã dừng được giữ thêm `buffer/finished_retention_sec` giây (mặc định
  900) để client còn đọc được trạng thái cuối và các bộ đếm, rồi bị quên đi.
  Việc dọn chạy trên luồng `main` mỗi 5 giây.
- `SessionRef` là `std::shared_ptr`. Một handler có thể đang giữa chừng một
  `EnrollSpeaker` 120 giây trên luồng này trong khi bộ dọn quyết định trên luồng
  kia rằng một phiên đã hết hạn; con trỏ trần ở đó là một use-after-free đang
  chờ một ngày bận rộn.
- `SIGINT`/`SIGTERM` chỉ đặt một cờ `sig_atomic_t`; một `QTimer` 200 ms đọc cờ
  đó rồi gọi `quit()`. Gần như không có gì hợp lệ bên trong một signal handler
  — kể cả `QCoreApplication::quit`, kể cả ghi log, kể cả cấp phát bộ nhớ.
- Thứ tự tắt: **`BufferHub::shutdown()` trước, `Server::stop()` sau.** Dừng các
  vòng đẩy trước sẽ đánh thức mọi handler đang chờ drain, để các luồng kết nối
  kịp trả lời thay vì bị cắt giữa chừng.
- `applog::shutdown()` chỉ được gọi **sau khi** `BufferHub` và `grpc::Server` đã
  bị huỷ. Cả hai đều ghi log trong lúc dừng, và một lần thoát sớm ở đường "không
  mở được cổng" từng ghi vài dòng dọn dẹp *sau* dòng "logger stopping" — xem chú
  thích ngay tại chỗ trong `s2t-qt-server/main.cpp`.

---
## 8. Tầng giao thức

Không có `protoc`, không có `Qt::Grpc`, không có thư viện gRPC/protobuf. Cả
tầng này được viết tay để hai chương trình build được chỉ với Qt. Nó nằm trong
`shared/` và **cả hai bên đều dùng** — nhưng mỗi bên dùng một nửa khác nhau,
và từ khi tách thì cả hai nửa đều tồn tại:

| | ghi | đọc |
|---|---|---|
| **request** | `serialize()` — client | `parse()` — server |
| **response** | `serialize()` — server | `parse()` — client |

Trước khi tách chỉ có nửa bên trái của mỗi hàng. Nửa còn lại được viết tay theo
đúng những số hiệu trường ấy, cố ý viết lại chứ không sinh ra bằng macro: số
hiệu trường là hợp đồng với bên ngoài, và một hợp đồng phải bung ra trong đầu
mới đọc được là hợp đồng không ai kiểm.

`s2t-qt-server --selftest-codec` chạy vòng tròn theo cả hai chiều, kể cả một
phép khẳng định rằng `DisplayRow` **không bao giờ** ghi hai trường 7 và 9 đã
được giữ chỗ.

### 8.1 Chiều gọi đi (cả hai chương trình)

```
AsrClient          đường method đúng như trong .proto
   │               ("/asr.ui.v1.ProductASRService/push_audio", ...)
   ▼
grpc::Channel      khung length-prefix (1 byte cờ nén + 4 byte độ dài)
   │               header: :method :scheme :path :authority, te: trailers,
   │               grpc-timeout, authorization: Bearer ...
   │               đọc grpc-status ở trailer, hoặc ở header nếu là
   │               trailers-only (dạng thường gặp của một lỗi)
   ▼
http2::Http2Connection   preface, SETTINGS, WINDOW_UPDATE, DATA, GOAWAY,
   │                     RST_STREAM, CONTINUATION
   ▼
hpack::Encoder/Decoder   bảng tĩnh + bảng động, Huffman
```

Vài lựa chọn đáng nhớ:

- **Đồng bộ (blocking) có chủ ý.** Mọi bên gọi đều đã có luồng riêng, nên một
  socket đồng bộ làm cho tầng này còn ~400 dòng thay vì một máy trạng thái
  callback.
- **Cửa sổ nhận ban đầu 8 MiB.** `SETTINGS` không nâng được cửa sổ mức kết nối
  (chỉ mức stream), nên phải gửi thêm `WINDOW_UPDATE` tường minh, nếu không một
  phản hồi lớn sẽ nghẽn ở 64 KiB bất kể `INITIAL_WINDOW_SIZE` là bao nhiêu.
- **Trần phản hồi 64 MiB.** `get_review_state` của một cuộc họp dài là thứ lớn
  nhất client từng đọc; bản thân adapter đã chặn ở 4 MiB của gRPC.
- **Deadline truyền tải = deadline gRPC + 2 giây.** Để một server có tôn trọng
  `grpc-timeout` kịp trả về `DEADLINE_EXCEEDED` thật, thay vì bị ta phá socket
  trước rồi báo thành lỗi kết nối.
- **Header block cho stream lạ là lỗi chí mạng.** Trạng thái HPACK thuộc về cả
  kết nối chứ không riêng từng stream; bỏ qua một header block của stream khác
  sẽ làm lệch bộ giải mã cho mọi phản hồi về sau.

### 8.2 Chiều nhận (chỉ `s2t-qt-server`)

```
http2::Http2Server    QTcpServer nhận, mỗi kết nối một QThread
   │                  preface của server gửi *trước* khi đọc preface của client
   │                  (để một client chờ preface của server không thể bế tắc)
   ▼
ServerConnection      vòng lặp chặn: đọc khung → gom header block → khi
   │                  END_STREAM thì gọi handler → ghi HEADERS, DATA, trailers
   ▼
grpc::Server          kiểm token, phân giải đường method, bóc khung
   │                  length-prefix, gọi handler, ghi grpc-status vào trailer
   ▼
BufferService         4 handler có đệm, 16 handler chuyển tiếp, 3 handler quản trị
```

Vài lựa chọn đáng nhớ, đối xứng với danh sách trên:

- **Luôn trả `:status 200`.** gRPC mang trạng thái của riêng nó trong trailer;
  một mã HTTP khác 200 chỉ khiến client đúng chuẩn báo là lỗi truyền tải thay
  vì báo đúng trạng thái mà handler đã chọn.
- **Lỗi trả bằng dạng trailers-only** — một khung HEADERS mang cả header lẫn
  `grpc-status`, không có DATA. Đó là hình dạng một server gRPC thật dùng, và
  cũng đúng là hình dạng `Http2Client` sẵn có biết đọc trạng thái ra.
- **`grpc-message` được mã hoá phần trăm.** Không có nó thì một thông báo lỗi
  tiếng Việt tới tay client thành ký tự rác.
- **DATA không bao giờ mang `END_STREAM`.** Khung HEADERS trailer mới là thứ
  đóng stream, vì nó chở `grpc-status`.
- **Số stream mở dở bị chặn (64).** Mỗi stream giữ một request body đang gom;
  không chặn thì một peer mở stream rồi không bao giờ kết thúc là một cách làm
  cạn bộ nhớ, chứ không phải một client chậm.
- **Header block cho stream lạ vẫn phải được giải mã.** Trạng thái HPACK thuộc
  về cả kết nối; bỏ qua một block sẽ làm lệch bộ giải mã cho mọi request về
  sau. Nên stream bị từ chối bằng `RST_STREAM`, còn block thì vẫn được giải mã.
- **Quá giới hạn kết nối thì từ chối ồn ào** bằng `GOAWAY(REFUSED_STREAM)`.
  Nhận rồi bỏ đói sẽ khiến điều hành viên thấy y như mất mạng, và đó đúng là
  thứ sai để đi tìm.
- **`grpc-timeout` của client được đọc ra và chuyển tiếp nguyên.** Một người
  gọi đã bỏ cuộc thì không nên còn được chờ ở phía trên.

Danh sách đầy đủ 20 RPC, số hiệu từng trường của mọi thông điệp, mã lỗi, chính
sách thử lại và một tệp `.proto` tái dựng nằm ở
[danh-sach-api.md](danh-sach-api.md). Khi sửa bất cứ thứ gì trong
`shared/proto/` hay `shared/grpc/`, tài liệu đó phải được cập nhật cùng lúc —
nó là thứ duy nhất người ngoài đọc.

---

## 9. Đường ghi: RpcExecutor

Mọi RPC theo yêu cầu đi qua đây:

```cpp
rpc()->call<ResponseType>(
    receiver,
    [request](AsrClient &client, ResponseType &out) { ... },   // chạy trên lane
    [this](const grpc::Status &status, const ResponseType &r) { ... });  // trên
                                                    // luồng của receiver
```

Nếu `receiver` bị huỷ trước, Qt bỏ kết quả đã xếp hàng — một hộp thoại đóng
giữa chừng không thể bị ghi vào sau khi đã bị xoá.

Deadline theo loại việc: sửa văn bản và đổi tên người nói 60 s; lấy audio, lưu
speaker của phiên và `EnrollSpeaker` 120 s (cái cuối bao trọn một lượt
rebuild_db trên toàn bộ giọng đã có, không chỉ giọng vừa ghi).

### Đồng thuận khi sửa

Mỗi `apply_text_edit` mang theo `base_revision`. Server từ chối nếu revision đã
nhích, và bảng soát lại tự tải lại — không có chuyện ghi đè âm thầm. Lỗi
`edit_range_not_committed` nghĩa là đoạn đó còn là kết quả tạm, chưa chốt.

---

## 10. Ba trạng thái của `expected_speakers`

`config_json` của `start_session` phân biệt ba thứ, và sự khác nhau là có chủ ý:

| Trạng thái | Trên dây | Nghĩa |
|---|---|---|
| Không giới hạn | khoá bị **bỏ hẳn** | So khớp với toàn bộ registry chung |
| Giới hạn, danh sách rỗng | khoá có mặt, mảng rỗng | **Không gán** tên đã đăng ký nào |
| Giới hạn, có tên | khoá có mặt, có phần tử | Chỉ những tên đó tới được bộ xác thực |

Một mảng rỗng tường minh **không** quay về nghĩa thứ nhất.

---

## 11. Hệ thống log

```
LOG_INFO(applog::cat::Worker) << "start_session OK - session_id=" << id;
        │
        ▼
applog::Record  (dựng trên stack, phát ra ở destructor)
        │
        ├──► sink: console (Debug) hoặc tệp xoay vòng (Develop)
        ├──► vòng đệm ~4000 dòng gần nhất
        └──► applog::Bus ──queued signal──► DiagnosticsWindow (luồng GUI)
```

- **Cờ chế độ** quyết định *đi đâu*; **mức** quyết định *bao nhiêu*. Hai thứ
  độc lập.
- Thứ tự quyết định chế độ: `--log-mode` → `S2T_LOG_MODE` → cấu hình đã lưu →
  mặc định lúc build (`qmake CONFIG+=develop`). Một lần `setMode()` sau đó — tức
  người dùng đổi trong ứng dụng — luôn thắng.
- Sink có mutex bảo vệ: capture, worker, poller và ba lane đều ghi từ luồng của
  mình. Tín hiệu lên GUI được phát **ngoài** phạm vi khoá, để một slot có ghi
  log cũng không tự khoá chính nó.
- Mỗi dòng được flush ngay: lý do duy nhất để giữ tệp là vẫn còn đọc được vài
  dòng cuối sau khi sập nguồn.
- Vòng đệm được cắt **theo khối** (1000 dòng một lần, nên nó dao động khoảng
  4000–5000) chứ không cắt từng dòng: cắt một phần tử mỗi dòng sẽ memmove cả
  vector trên mỗi dòng, ngay dưới khoá sink và trên đường nóng của audio.
  `DiagnosticsWindow` cũng giữ vòng đệm riêng của nó theo đúng cách đó.
- Danh sách category và danh sách mức nằm **một chỗ duy nhất** — `cat::all()` và
  `selectableLevels()` trong `Logger.h`. Bộ lọc "tất cả thành phần" và các combo
  đều đọc từ đó, nên thêm một category mới không thể bị bỏ sót ở nơi khác.
- Macro dùng vòng `for` chạy đúng một lần, không dùng `if` — vì nó hay được viết
  làm thân của một `if` không ngoặc, và mọi khai triển có `if` ở đó đều vướng
  `-Wdangling-else`, thứ mà dự án này bật.

### Đường nóng được ghìm

| Chỗ | Cách ghìm |
|---|---|
| `push_audio` (~6 lần/giây) | Từng gói ở mức `trace`; mức `debug` chỉ tóm tắt mỗi 50 gói. |
| `get_live_state` (5 lần/giây) | `trace` từng lần; `debug` chỉ khi revision nhích. |
| Poll lỗi | Chỉ ghi lần đầu, im cho tới khi nội dung lỗi đổi hoặc hết lỗi. |
| Đèn kết nối (mỗi 3 giây) | Chỉ ghi khi trạng thái đổi. |
| Thử mở lại mic (2 lần/giây) | Mức `trace`. |

`grpc::Channel::invoke` là điểm chốt duy nhất mà **mọi** RPC đi qua, nên một
dòng ở đó bao trọn cả hơn 20 chỗ gọi mà không phải đụng vào chỗ nào.

---

## 12. Vòng đời khởi động và tắt

**Khởi động** (`main.cpp`):

1. Đặt tên tổ chức/ứng dụng — `QStandardPaths` suy ra thư mục log từ đây, và cả
   hai là hàm tĩnh nên chưa cần đối tượng ứng dụng.
2. `applog::initFromArguments()` — ngay sau đó, để mọi hỏng hóc ở các nhánh
   phía dưới đều đã được ghi lại.
3. Các chế độ headless (`--selftest`, `--selftest-net`, `--probe`) chạy trước
   khi có bất kỳ đối tượng GUI nào, nên dùng được qua ssh và trong CI.
4. Trên Unix: từ chối mở GUI nếu không có `DISPLAY`/`WAYLAND_DISPLAY`, và nói rõ
   phải làm gì — thay vì để `QApplication` tự abort bằng một thông báo
   platform-plugin không giúp được gì.
5. Đăng ký metatype → `MainWindow` → vòng lặp sự kiện. `MainWindow` được đặt
   trong một khối riêng để nó bị huỷ **trước** `applog::shutdown()`: quá trình
   tắt controller mất vài giây và ghi log suốt thời gian đó, nên gọi
   `shutdown()` trước sẽ đóng sink rồi để từng dòng sau nó phải mở lại tệp vừa
   đóng, dưới một dòng "logger stopping" chưa đúng sự thật.

**Tắt** (`~SessionController`):

1. `teardownWorker()` — yêu cầu dừng, ngắt kết nối signal, giao việc xoá cho
   vòng lặp sự kiện. Worker có thể còn đang trong một lần xả `stop_session` dài,
   nên không chặn GUI vào đó.
2. Chờ tối đa 10 giây cho worker còn treo: nó được parent vào controller nên
   `~QObject` sẽ xoá nó, mà xoá một `QThread` còn chạy là việc Qt không hỗ trợ —
   luồng sẽ tiếp tục chạy trên một object vừa bị giải phóng.
3. Dừng poller (chờ 3 giây), dừng capture (`BlockingQueuedConnection`, và chỉ
   khi luồng thật sự còn chạy — một lời gọi blocking vào một vòng lặp sự kiện đã
   dừng thì không bao giờ trở về).
4. `RpcExecutor` chờ mỗi lane 5 giây rồi thôi: lúc tắt máy, một socket kẹt không
   được phép giữ cả ứng dụng lại.

**Tắt** (`~DiagnosticsWindow`) — cửa sổ được parent vào cửa sổ chính và chỉ ẩn đi
khi đóng, nên tới đây nghĩa là ứng dụng đang thoát *trong lúc* một phép chẩn đoán
còn kẹt trên socket:

1. Ngắt kết nối signal tới widget — không gì được chạm vào nó nữa.
2. Chờ 20 giây. Probe thoát sớm sau RPC hỏng đầu tiên (~12 giây), nhưng bộ test
   mạng đầy đủ chạy tiếp qua từng deadline tới 30 giây một, nên không con số nào
   là đủ chắc — đó là lý do có hai bước dưới chứ không phải một số chờ lớn hơn.
3. Hết hạn thì `terminate()` (đúng luật mà worker, poller và ba lane đã theo:
   lúc tắt máy một socket kẹt không được giữ cả ứng dụng lại), rồi dọn bộ thu
   báo cáo của `SelfTest` — luồng bị giết không bao giờ chạy tới
   `captureReportInto(nullptr)` của chính nó, và biến toàn cục đó còn đang trỏ
   vào một `QString` trên stack đã chết.
4. Chỉ `delete` khi `isFinished()`. Nếu cả `terminate()` cũng không ăn thì
   `setParent(nullptr)` rồi bỏ rơi: rò một luồng lúc tiến trình sắp thoát vẫn
   hơn là phá tiếp.

---

## 12b. Tầng giao diện

Trước 2026-08-29 giao diện không có tầng nào cả: chín tệp `ui/` mỗi tệp tự viết
`setStyleSheet("font-family:monospace; font-size:11px; border:1px solid #d0d0d0")`
với mã màu chọn từng chỗ một. Hệ quả là cùng một loại widget có ba viền khác
nhau tuỳ cửa sổ, và một desktop nền tối vẫn bị vẽ chữ xám đậm lên những hộp
trắng cứng.

`ui/Theme.{h,cpp}` giờ là **nơi duy nhất** quyết định ứng dụng trông thế nào.
`main()` gọi `theme::apply(app)` ngay sau khi dựng `QApplication`, trước widget
đầu tiên.

| Thứ | Lấy từ đâu |
|---|---|
| Màu | `theme::color(Role)` — vai trò ngữ nghĩa (`Surface`, `Border`, `TextMuted`, `Danger`…), có sẵn hai bảng sáng/tối |
| Màu làn người nói | `theme::laneColor(i)` |
| Kiểu chữ đẳng khoảng | `theme::mono()` |
| Biểu tượng | `theme::icon(Glyph)` — vẽ bằng `QPainter`, không có tệp ảnh |
| Phần còn lại | style sheet toàn ứng dụng dựng trong `buildStyleSheet()` |

Bốn ràng buộc, và chúng là lý do tầng này tồn tại:

- **Không viết mã màu thẳng trong tệp widget.** Thêm một `Role` mới ở đây, để
  bảng màu tối cũng nhận được nó.
- **Không viết cỡ chữ theo pixel.** Cùng một nhãn tiếng Việt đo rộng hơn
  khoảng 90 px trên bộ phông của RHEL so với bộ của MinGW; một cỡ pixel vừa
  trên kit này thì tràn trên kit kia. Dùng bội số của phông desktop.
- **Style là Fusion trên cả hai kit, cố ý.** Style Windows 11 bỏ qua phần lớn
  style sheet và mang theo metric riêng, nên giữ nó nghĩa là cùng một chương
  trình trông như hai sản phẩm khác nhau, và mọi lần kiểm tra layout trên kit
  này không nói được gì về kit kia.
- **Không có `.qrc`, và đó là chủ ý** (xem lý do ở phần giao thức: cả dự án
  build được chỉ với Qt). Vì vậy biểu tượng được *vẽ*, còn mũi tên của
  `QComboBox`/`QSpinBox` — thứ style sheet bắt buộc phải có ảnh mới vẽ được —
  được ghi ra `AppDataLocation/ui/*.png` lúc khởi động rồi trỏ tới bằng đường
  dẫn tuyệt đối. Nếu ghi không được thì phần luật đó bị bỏ và style nền tảng
  tự vẽ mũi tên của nó: xấu hơn, không bao giờ trống.

`theme::apply()` cũng cài một `QTranslator` không đọc tệp nào, chỉ trả lời cho
ngữ cảnh `QPlatformTheme`. Đó là chỗ Qt lấy chữ cho nút chuẩn, nên nó dịch
"Save"/"Cancel"/"Close" của **mọi** `QDialogButtonBox` và `QMessageBox` sang
tiếng Việt một lần, kể cả những hộp thoại chưa viết.

**Cửa sổ chính chia việc theo người dùng, không theo widget:**

- `MainWindow::buildActions()` dựng mỗi `QAction` **một lần**; menu và thanh
  công cụ cùng tham chiếu tới nó. Hai `QAction` làm cùng một việc là cách một
  nút bật/tắt lệch pha với thứ nó điều khiển.
- Thanh công cụ chỉ giữ những việc hay dùng giữa cuộc họp. Sáu cửa sổ công cụ
  nằm ở menu — nhồi hết lên thanh công cụ chính là thứ từng đẩy nút "Cấu hình"
  vào menu tràn "»" đúng lúc mất kết nối và người vận hành cần nó.
- Trạng thái sống (micro, đèn kết nối) ở đầu phải thanh công cụ; danh tính và
  mã phiên ở thanh dưới.
- `ui/StatusPanel` tách số liệu theo *ai hỏi*: một con số lớn cho người vận
  hành, bốn dòng chỉ ra thời gian đi đâu, còn phân vị và bộ đếm mô hình nằm sau
  nút "Chi tiết kỹ thuật".

---

## 13. Bản đồ mã nguồn

| Thư mục | Nội dung |
|---|---|
| `shared/proto/` | `ProtoWire` (proto3 tay, hai chiều), `AsrSession`, `SpeakerRegistry`, `BufferAdmin` — struct chép tay theo `.proto` |
| `shared/grpc/` | `Hpack`, `Http2Client`, `GrpcChannel`, `AsrClient` (gọi đi); `Http2Server`, `GrpcServer` (nhận); `Methods.h` (đường method dùng chung cho cả hai bên) |
| `shared/core/` | `Logger` — hệ thống nhật ký, chung cho cả hai chương trình |
| `s2t-qt-server/` | `main`, `ServerConfig`, `RpcLane`, `SessionJournal` (hàng đợi trên đĩa cho việc khôi phục), `SessionStore` (bản lưu cuộc họp: SQLite + một tệp `.s16le` mỗi họp), `CampPlusClient` (HTTP/1.1 tới dịch vụ đăng ký giọng), `LiveTranscript` (bản chép dựng tại chỗ), `SessionBuffer`, `BufferHub`, `BufferService`, `ServerSelfTest` |
| `s2t-qt-server/backend/` | `InferenceBackend` (ranh giới), `TritonBackend` (KServe v2), `RivaBackend` (`nvidia.riva.asr`) |
| `s2t-qt-client/core/` | `SessionController` (mặt tiền), `SessionWorker`, `StatePoller`, `RpcExecutor`, `AudioQueue`, `TranscriptModel`, `AppConfig`, `SelfTest` |
| `s2t-qt-client/audio/` | `AudioCapture`, `MicDenoise` (điều khiển xvf3800), `WavIo`, `Transcode` (ffmpeg) |
| `s2t-qt-client/ui/` | `Theme` (mã màu, kiểu chữ, biểu tượng, style sheet toàn ứng dụng), `StatusPanel` (cột phải của cửa sổ chính), `TimelineView`, `Dialogs`, `ReviewPanel`, `EnrollDialog`, `TraceWindow`, `EvidenceWindow`, `DiagnosticsWindow`, `SubtitleWindow`, `LogControls` (combo chế độ/mức log dùng chung) |
| `tools/` | `mock_adapter.js` (peer HTTP/2 độc lập cho `--selftest-net`), `build_rhel9.sh`, `deploy_rhel.sh` (đẩy mã nguồn + build + `--selftest` lên máy RHEL), `run_valgrind.sh`, `valgrind.supp`, `s2t-qt-server.service`, `s2t-qt-server.conf.sample`, `interop_check.py` (grpc thật gọi vào server này), `restart_check.py` (SIGKILL giữa cuộc họp) |
| `docs/` | `huong-dan-su-dung.md` (vận hành), `luong-hoat-dong.md` (tài liệu này), `danh-sach-api.md` (hợp đồng gRPC cho client bên ngoài), `slide.pdf` (mô tả kiến trúc gốc) |

`shared/` được nạp bằng `include(shared/shared.pri)` hoặc
`include(shared/server.pri)` vào từng `.pro` chứ không build thành thư viện
tĩnh: hai kit khi đó không phải thống nhất thứ tự link, và không có bước build
nào phải nhớ. Đổi lại phần dùng chung được biên dịch hai lần.

`server.pri` = `shared.pri` + `Http2Server` + `GrpcServer`. Client **không**
lấy hai tệp đó: nó không bao giờ mở cổng lắng nghe, nên nó không nên chứa một
máy chủ HTTP/2 chưa từng được gọi tới.

### Build

```bash
# Windows, kit mingw_64
set PATH=C:\Qt\6.11.2\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;%PATH%
qmake s2t_qt.pro && mingw32-make -j8          # cả hai (TEMPLATE = subdirs)

# RHEL 9 — dùng qmake6, không phải qmake (qmake ở đó là của Qt 5)
tools/build_rhel9.sh            # cả hai
tools/build_rhel9.sh server     # chỉ server: không cần Qt Multimedia/Widgets
tools/build_rhel9.sh client
```

Cờ build:

| Cờ | Tác dụng |
|---|---|
| `CONFIG+=develop` | Mặc định log ra tệp thay vì console |
| `CONFIG+=memcheck` | `-O1 -g3 -fno-omit-frame-pointer -rdynamic`, cho gdb/valgrind |

Cả hai dự án build sạch với `-Wall -Wextra` và điều đó được đặt ngay trong
`.pro` chứ không nằm trong lời văn, để nó bị kiểm tra ở mọi lần build — trên cả
GCC 13.1 (MinGW) lẫn gcc 11.5 (RHEL 9).

### Kiểm thử

```bash
# server: bộ mã hai chiều + loopback thật + cả chuỗi client→đệm→tầng suy luận
s2t-qt-server --selftest
s2t-qt-server --selftest-codec                       # chỉ bộ mã, không socket
s2t-qt-server --probe 192.168.1.47:8011 --token T    # tầng suy luận có sống không

# client
s2t-qt-client --selftest                             # proto3 + HPACK, không mạng
node tools/mock_adapter.js 18700 &                   # peer HTTP/2 độc lập
s2t-qt-client --selftest-net 127.0.0.1:18700 --token T
s2t-qt-client --probe 192.168.1.47:8800 --token T    # chẩn đoán tại hiện trường
```

Ba chế độ của client đều gọi được từ tab **Chẩn đoán** trong cửa sổ *Nhật ký &
Chẩn đoán*, cùng nút **Đọc trạng thái đệm** hiển thị `get_buffer_status`.

`s2t-qt-server --selftest` là bài đáng giá nhất: nó dựng một tầng suy luận giả,
đặt `BufferHub` và `BufferService` **thật** trước nó, rồi lấy một client thật
lái cả hai — nên nó kiểm được những thứ chỉ riêng bộ đệm mới có thể làm sai:
thứ tự gói, rào chắn drain, phát lại ACK, bộ đệm trạng thái, và việc khôi phục
sau khi khởi động lại.

Hai bài nữa cần `python3` + `grpcio-tools` (máy RHEL có sẵn), và cả hai đều
kiểm được những thứ mà một bài tự-kiểm không thể:

```bash
python3 tools/interop_check.py 127.0.0.1:8800 <token>
python3 tools/restart_check.py ./s2t-qt-server [--durability fsync]
```

- `interop_check.py` cho **grpc C-core thật** gọi vào server tự viết. Client và
  server ở đây dùng chung một bản HPACK và một bộ mã proto3, nên chúng có thể
  đồng ý với nhau mà cả hai cùng sai; grpc C-core thì không đồng ý với ai cả.
  Stub được sinh từ chính `.proto` in trong `danh-sach-api.md`, nên nó cũng
  kiểm luôn tài liệu.
- `restart_check.py` **giết máy chủ bằng `SIGKILL`** giữa cuộc họp. Bài
  `--selftest` chỉ tháo đối tượng, nên nó không chứng minh được điều quan trọng
  nhất: rằng bản ghi đã nằm ngoài tiến trình trước khi client được ACK.
  `SIGKILL` không chạy destructor và không flush gì cả.

---

## 14. Những ràng buộc không được phá

Đây là các quyết định mà việc "dọn dẹp" sẽ làm hỏng hành vi:

1. **Mở thiết bị trước, tạo phiên sau.** Đảo lại sẽ để phiên rỗng treo trên
   server.
2. **`stop_session` phải cùng luồng với vòng đẩy audio.** Đây là thứ bảo đảm nó
   đến sau gói cuối. Ràng buộc này **giờ tồn tại ở cả hai chặng**: trong
   `SessionWorker` của client và trong `SessionBuffer::run()` của server. Bỏ nó
   ở chặng nào cũng đủ để cắt cụt phần đuôi của một bản chép.
3. **`INTERNAL` không được thử lại.** Thử lại sẽ nhân đôi chữ. Cũng ở cả hai
   chặng — và bộ đệm phải **chuyển tiếp nguyên mã lỗi** cho client, chứ không
   được đổi nó thành một mã khác, nếu không quy tắc phía client sẽ kết luận
   sai.
4. **Hàng đợi có trần và dừng ầm ĩ.** Mở trần ra sẽ biến mất tiếng thành thứ
   không ai biết. Áp cho cả `AudioQueue` của client lẫn hàng đợi phiên của
   server.
5. **Tạm dừng bỏ dữ liệu lúc push.** Bỏ lúc gửi sẽ giao lời nói lúc tạm dừng như
   thể nó là trực tiếp.
6. **`expected_speakers` là ba trạng thái.** Gộp mảng rỗng vào "bỏ khoá" sẽ đổi
   nghĩa.
7. **`operatorId` không được lưu.** Nó là người chịu trách nhiệm trong nhật ký
   kiểm toán.
8. **Ràng buộc tên thiết bị.** Bỏ đi sẽ thu nhầm thiết bị sau khi rút USB mic.
9. **Không thêm phụ thuộc protobuf/gRPC.** Cả tầng giao thức viết tay tồn tại là
   để ứng dụng build được chỉ với Qt trên một máy trạm đã triển khai.
10. **Số hiệu trường trong `shared/proto/` là hợp đồng, không phải chi tiết cài
    đặt.**
    Kể từ khi [danh-sach-api.md](danh-sach-api.md) tồn tại, chúng còn là hợp
    đồng với những client không nằm trong kho mã này. Đổi một số hiệu, hoặc tái
    sử dụng số 7 và 9 đã `reserved` của `DisplayRow`, là làm hỏng âm thầm mọi
    bên đang chạy — proto3 không báo lỗi, nó chỉ đọc ra giá trị khác.
11. **`DiagnosticsRunner` không được xoá khi còn chạy.** `runProbe` và
    `runNetworkTests` nhận `const QString&`, còn `run()` truyền thẳng
    `m_target`/`m_token` — tức chúng giữ tham chiếu vào chính object runner suốt
    hàng chục giây. Xoá nó giữa chừng là use-after-free đọc ruột `QString`, âm
    thầm chứ không nổ ra ngay. Vì vậy `onJobCompleted` **không** gọi
    `deleteLater()` (slot đó chạy từ câu lệnh cuối của `run()`, luồng chưa thật
    sự thoát); runner được giữ lại tới lần chạy sau hoặc tới destructor.
12. **`ACK` của `push_audio` nghĩa là "đã nằm chắc trong bộ đệm", và
    `sourceSeenSec` trong ACK vẫn là con số của *tầng suy luận*.** Nếu bộ đệm
    tự điền vào đó số byte nó đã nhận, "hàng đợi server AI" trên màn hình điều
    hành viên sẽ luôn bằng 0 và độ trễ thật của đường ống trở nên vô hình.
13. **`QTcpSocket` không được dùng chéo luồng.** `grpc::Channel` mở socket một
    cách lười, ngay trong lời gọi đầu tiên, nên "một kênh dùng chung" nghĩa là
    "một socket tạo ở luồng này, đọc ở luồng kia". Đó là lý do `RpcLane` tồn
    tại. Bỏ nó đi để "cho gọn" sẽ chạy được trong thử nghiệm và hỏng khi tải
    lên.
14. **Thứ tự khoá trong `SessionBuffer` là `m_stateMutex` rồi `m_mutex`.**
    `snapshot()` cần cả hai; đảo thứ tự ở một chỗ là đủ để có deadlock.
15. **`applog::shutdown()` phải chạy sau khi `BufferHub` và `grpc::Server` đã
    bị huỷ.** Cả hai đều ghi log trong lúc dừng. Một đường thoát sớm ở nhánh
    "không mở được cổng" từng vi phạm điều này và ghi vài dòng dọn dẹp *sau*
    dòng "logger stopping".
16. **Bộ đệm không được chuyển tiếp `push_audio` cho một phiên nó không giữ.**
    Trả `NOT_FOUND` kèm lý do. Chuyển tiếp mù sẽ làm mọi bộ đếm — độ trễ, số
    gói, số byte — nói dối, và đó là những con số duy nhất cho biết đường ống
    có theo kịp hay không.
17. **`grpc::Server` luôn trả `:status 200`, và lỗi đi bằng dạng
    trailers-only.** Đổi sang một mã HTTP khác sẽ khiến mọi client đúng chuẩn
    báo lỗi truyền tải thay vì báo đúng trạng thái gRPC.
18. **Bản ghi `Packet` phải nằm trên đĩa trước khi client được ACK, và bản ghi
    `Progress` phải nằm sau khi đẩy lên tầng suy luận thành công.** Đảo quy tắc
    thứ nhất thì ACK thành lời hứa suông; đảo quy tắc thứ hai thì một cú chết
    đúng lúc làm mất hẳn một gói. Xem mục 7b.7.
19. **Ghi nhật ký hỏng là từ chối gói, không phải ghi cảnh báo.** Một phiên âm
    thầm hạ cấp xuống "không bền" nói dối client về ý nghĩa của ACK; một phiên
    từ chối thẳng thì không. Với `Progress` thì ngược lại — mất nó chỉ tốn một
    lần gửi lại, nên đó mới là chỗ dùng cảnh báo.
20. **Bản ghi rách ở cuối nhật ký là bình thường.** Đó là hình dạng của một
    nhật ký sau đúng cái sự cố mà nó sinh ra để lo. Coi nó là hỏng và từ chối
    khôi phục sẽ vứt bỏ cả cuộc họp vì mười ba byte cuối.
21. **Bộ dọn chạy trên luồng nhận kết nối, nên nó không được chờ bất cứ thứ gì
    trên mạng.** Đóng một phiên mồ côi là `requestStop()` chứ không phải
    `stop()`.
22. **Một thư mục nhật ký chỉ được một tiến trình dùng.** Hai máy chủ dùng
    chung sẽ cùng khôi phục một phiên và cùng gửi lại một gói, nhân đôi chữ.
    `BufferHub` giữ một `QLockFile` ở đó và từ chối khởi động nếu đã có ai giữ.
    Phải là `QLockFile` chứ không phải một tệp `O_EXCL`: nó ghi pid và chiếm
    lại khoá của một tiến trình đã chết, nên `SIGKILL` không để lại khoá chết
    chặn mất đúng cái lần khởi động lại mà nhật ký sinh ra để phục vụ.
23. **Danh tính người nói phải đi cùng các từ của gói, và đường sửa của điều
    hành viên không được mang danh tính nào.** Trong `LiveTranscript`,
    `replaceSpan()` (gói từ tầng suy luận) và `applyEdit()` (điều hành viên sửa
    chữ) cùng dùng chung `spliceWords()`, nhưng khác nhau đúng ở ba tham số
    `speaker` / `speakerProb` / `verifiedName`. Gói **có** ba thứ đó và phải
    truyền xuống; thao tác sửa **không có**, và mỗi từ giữ nguyên làn cũ của nó
    — nếu không thì sửa một câu sẽ gán nó cho người khác.

    Đây là một lỗi có thật, phát hiện 2026-09-02: `replaceSpan()` gọi thẳng
    `applyEdit()`, mà `asr_words` thì **luôn** có mốc thời gian, nên đó là
    đường đi của *mọi* gói. Hệ quả là tên người nói mà `asr_diar_session` trả
    về (đo được: `verified_name="Anna"`, `verify_score` 0,84) bị vứt sạch, và
    mọi dòng đã chốt hiện ra là "Người 1". Rất khó thấy, vì hàng tạm được gán
    thẳng trong `apply()` nên **vẫn** có tên đúng.
24. **Correction sửa chữ, không sửa người nói.** `merged_words` của một
    correction thường trải dài hàng phút, nên đóng dấu `speaker` của gói hiện
    tại lên cả khoảng đó sẽ xoá sạch phân vai của mọi câu nó phủ. Vì vậy
    `apply()` gọi `replaceSpan(correction.mergedWords, QString(), 0, QString())`
    — không mang danh tính nào — và mỗi từ được sửa thừa kế làn + tên của **đúng
    từ nó thay thế**, tra theo thời gian (`priorAt()`), chứ không phải của từ
    đầu tiên trong khoảng: một correction có thể phủ nhiều lượt nói, gán tất cả
    cho người đầu tiên là gộp những người vốn đã được tách đúng.

    Đo được ngày 2026-09-02 trên mẫu 5 phút: dòng ở 00:21 lần lượt mang nhãn
    Newsman → Anna → (không tên) → "Người 4" — vẫn đúng những chữ ấy, bốn người
    khác nhau, chỉ vì các correction lăn qua nó.
25. **Người nói của một từ tra theo mốc thời gian của chính nó, không theo
    `PushAudioResponse.speaker`.** Trường `speaker` mô tả **gói vừa được giải
    mã**, còn các từ trong gói đó đã được nói trước đó vài giây — ASR trả chữ
    trễ hơn lời nói. Trong một buổi phỏng vấn, khoảng trễ ấy đủ để câu hỏi rơi
    sang người trả lời.

    Nguồn đúng là `diar_chunk_preds_flat` + `diar_chunk_preds_shape` +
    `diar_subframe_start_ms/end_ms` — ma trận điểm `[số lát x số người]` kèm
    mốc của từng lát, **vốn đã có sẵn trên đường truyền** trong
    `asr::Diarization` và trước 2026-09-02 không ai dùng.
    `LiveTranscript::foldDiarization()` gộp chúng thành các lượt nói (các lát
    liên tiếp cùng người được nhập một), `speakerAt()` trả về lượt phủ nhiều
    nhất khoảng `[start, end]` của từ. Ngưỡng `kDiarFloor = 0.5`: dưới mức đó
    không ai đủ rõ để nhận lát ấy, và im lặng thì tốt hơn là bịa ra một lượt
    nói từ tiếng ồn nền.

    Kéo theo: **tên đã xác minh của gói chỉ áp cho làn của gói đó**
    (`word.speaker == chunkSpeaker`). Khi một từ có thể rơi vào làn khác với
    làn đang được giải mã, gán bừa tên ấy là đặt tên người đang nói lên lời của
    người đã nói.

    Kiểm chứng 2026-09-02, mẫu phỏng vấn 2:33: trước khi sửa, câu mở đầu của
    người phỏng vấn bị cắt đôi và nửa sau gán cho ứng viên; sau khi sửa, cả câu
    nằm đúng dưới `Interviewer`.
26. **Làn của một từ được đóng băng ở lần xếp chỗ đầu tiên.** `asr_words` là
    cửa sổ trượt rộng khoảng tám giây, còn mỗi gói chỉ mang **một** giá trị
    `speaker` cho toàn bộ cửa sổ đó. Đóng dấu giá trị ấy lên cả lô nghĩa là mọi
    ranh giới lượt nói nằm trong tám giây bị gán lại cho người đang nói ở thời
    điểm hiện tại. Trong một buổi phỏng vấn — nơi các lượt cách nhau vài giây —
    hậu quả là câu hỏi bị gán cho người trả lời.

    Vì vậy `speaker` của gói chỉ áp cho những từ **cửa sổ chưa từng xếp chỗ**
    (`placedAt()` — so khớp theo bao hàm thời gian, không phải "từ gần nhất
    phía trước", nếu không một từ hoàn toàn mới sẽ bị đóng băng vào làn của
    người nói trước đó). Tên cũng theo quy tắc ấy, và chỉ được nâng cấp **một
    lần**, từ "chưa có tên" sang một tên đã xác minh.
    `s2t-qt-client/core/TranscriptModel.cpp` đóng băng làn y hệt và nói rõ lý
    do — server giờ giữ trạng thái nên phải theo cùng luật.

    Đo được ngày 2026-09-02 trên mẫu phỏng vấn 2:33: "điểm mạnh của bạn là gì"
    rơi vào ứng viên, còn câu trả lời của cô ấy rơi vào người phỏng vấn.
27. **Tên đi theo từng từ, không đi theo làn người nói.** Trong
    `spliceWords()`, mỗi từ giữ cái tên nó được gán lúc nó đến (`Placed::name`),
    y như `_verified_name` trên mỗi word của `grpc_session_adapter.py`. Gắn tên
    theo làn thì mỗi câu trả lời mới cho làn đó sẽ **viết lại quá khứ**: tầng
    suy luận liên tục đổi ý giữa các ứng viên nó đang cân nhắc, nên một cuộc
    họp mà Newsman nói trước sẽ bị đổi hết lời của anh ta thành người được gọi
    tên sau cùng. Đo được ngày 2026-09-02: cả 8 dòng đầu bị gán lại thành
    "Cowoker".
28. **Tên giữ chỗ không được đè lên tên thật.** Tầng suy luận trả
    `verified_name="unknown"` xen kẽ với tên thật suốt cuộc họp, nên để lọt sẽ
    làm tên nhấp nháy mất vài trăm mili-giây một lần.
    `LiveTranscript::isPlaceholderName()` giữ danh sách các cách nói "chưa nhận
    ra ai", và nó phải **khớp** với `TranscriptModel::isRealName()` bên client:
    bên này quyết định cái gì được *lưu*, bên kia quyết định cái gì được *vẽ*,
    lệch nhau thì sinh ra những dòng trông như vô danh mà không ai giải thích
    được.
29. **`speaker = -1` không phải là người nói số -1.** Đó là "diarization chưa
    quyết được". Dựng nó thành một làn riêng sẽ đẻ ra người nói ma "Người 0" ở
    client, vì `DisplayRow.speaker` được client đọc bằng `toInt()`. Các từ đó
    chờ cửa sổ trượt gửi lại kèm làn thật.
