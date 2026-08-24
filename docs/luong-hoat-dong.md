# s2t_qt — Luồng hoạt động

Tài liệu này mô tả bên trong ứng dụng: các luồng (thread), các kênh kết nối,
vòng đời một phiên, và lý do đằng sau những chỗ trông có vẻ lạ. Dành cho người
bảo trì mã nguồn. Phần dành cho người vận hành nằm ở
[huong-dan-su-dung.md](huong-dan-su-dung.md).

---

## 1. Toàn cảnh

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
                                  AI server (gRPC adapter)
```

### Vì sao nhiều kênh như vậy

Một `grpc::Channel` ứng với đúng một kết nối TCP và thuộc về đúng một luồng.
Việc đẩy audio, việc poll trạng thái và các RPC theo yêu cầu **không dùng chung
kênh**:

- `get_live_state` phình theo độ dài cuộc họp. Nếu nó dùng chung kênh với audio
  thì một lần tuần tự hoá trạng thái lớn sẽ nằm chắn trước một gói audio 160 ms.
- Một `EnrollSpeaker` được phép chạy tới 120 giây. Ba lane là con số nhỏ nhất
  giữ cho nó không chắn cả bộ chọn phiên lẫn lần poll trạng thái model phía sau.

Đây cũng chính là hình dạng mà cầu nối Python trước đây dùng.

---

## 2. Các luồng

| Tên luồng | Tạo bởi | Việc |
|---|---|---|
| `main` | Qt | Toàn bộ widget. Không bao giờ chặn ở I/O mạng. |
| `audio-capture` | `SessionController` | `QAudioSource` + `mic::DenoiseAbRecorder`. Sống suốt vòng đời controller. |
| `session-worker` | `SessionController` | Một phiên trọn vẹn: `start_session` → vòng đẩy audio → `stop_session`. |
| `state-poller` | `SessionController` | `get_live_state` mỗi 200 ms. Sống suốt vòng đời controller; đổi phiên không tốn một lần kết nối lại. |
| `rpc-lane-0..2` | `RpcExecutor` | Các RPC theo yêu cầu: review, sửa, audio, đăng ký giọng, trace. |
| `diagnostics` | `DiagnosticsWindow` | Tạm thời, chỉ chạy trong lúc thực hiện một phép chẩn đoán. Mỗi lúc chỉ một cái: bộ thu báo cáo của `SelfTest` là biến toàn cục. |

Mọi thứ đi qua ranh giới luồng bằng signal/`invokeMethod` dạng queued đều phải
là metatype đã đăng ký, nếu không Qt lặng lẽ bỏ lời gọi lúc chạy (xem
`main.cpp`).

**Ngoại lệ có chủ ý:** tín hiệu `AudioCapture::chunk` nối bằng
`Qt::DirectConnection`. Nó chạy trên luồng capture và chỉ chạm vào `AudioQueue`
vốn đã có khoá riêng, nên audio không phải đi vòng qua vòng lặp sự kiện của GUI
để tới được bên gửi.

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

## 8. Tầng giao thức

Không có `protoc`, không có `Qt::Grpc`, không có thư viện gRPC/protobuf. Cả
tầng này được viết tay để ứng dụng build được chỉ với Qt.

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

## 13. Bản đồ mã nguồn

| Thư mục | Nội dung |
|---|---|
| `core/` | `SessionController` (mặt tiền), `SessionWorker`, `StatePoller`, `RpcExecutor`, `AudioQueue`, `TranscriptModel`, `AppConfig`, `Logger`, `SelfTest` |
| `audio/` | `AudioCapture`, `MicDenoise` (điều khiển xvf3800), `WavIo`, `Transcode` (ffmpeg) |
| `grpc/` | `AsrClient`, `GrpcChannel`, `Http2Client`, `Hpack` |
| `proto/` | `ProtoWire` (proto3 tay), `AsrSession`, `SpeakerRegistry` — struct chép tay theo `.proto` |
| `ui/` | `TimelineView`, `Dialogs`, `ReviewPanel`, `EnrollDialog`, `TraceWindow`, `EvidenceWindow`, `DiagnosticsWindow`, `LogControls` (combo chế độ/mức log dùng chung) |
| `tools/` | `mock_adapter.js` (peer HTTP/2 độc lập cho `--selftest-net`), `build_rhel9.sh`, `run_valgrind.sh` |

### Build

```bash
# Windows, kit mingw_64
set PATH=C:\Qt\6.11.2\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;%PATH%
qmake s2t_qt.pro && mingw32-make -j8

# RHEL 9 — dùng qmake6, không phải qmake (qmake ở đó là của Qt 5)
qmake6 s2t_qt.pro && make -j8
```

Cờ build:

| Cờ | Tác dụng |
|---|---|
| `CONFIG+=develop` | Mặc định log ra tệp thay vì console |
| `CONFIG+=memcheck` | `-O1 -g3 -fno-omit-frame-pointer -rdynamic`, cho gdb/valgrind |

Dự án build sạch với `-Wall -Wextra` và điều đó được đặt ngay trong `.pro` chứ
không nằm trong lời văn, để nó bị kiểm tra ở mọi lần build.

### Kiểm thử

```bash
s2t_qt --selftest                                # proto3 + HPACK, không mạng
node tools/mock_adapter.js &                     # peer HTTP/2 độc lập
s2t_qt --selftest-net 127.0.0.1:8700 --token T   # đầu-cuối thật
s2t_qt --probe 192.168.1.47:8700 --token T       # chẩn đoán tại hiện trường
```

Cả ba đều gọi được từ tab **Chẩn đoán** trong cửa sổ *Nhật ký & Chẩn đoán*.

---

## 14. Những ràng buộc không được phá

Đây là các quyết định mà việc "dọn dẹp" sẽ làm hỏng hành vi:

1. **Mở thiết bị trước, tạo phiên sau.** Đảo lại sẽ để phiên rỗng treo trên
   server.
2. **`stop_session` phải cùng luồng với vòng đẩy audio.** Đây là thứ bảo đảm nó
   đến sau gói cuối.
3. **`INTERNAL` không được thử lại.** Thử lại sẽ nhân đôi chữ.
4. **Hàng đợi có trần và dừng ầm ĩ.** Mở trần ra sẽ biến mất tiếng thành thứ
   không ai biết.
5. **Tạm dừng bỏ dữ liệu lúc push.** Bỏ lúc gửi sẽ giao lời nói lúc tạm dừng như
   thể nó là trực tiếp.
6. **`expected_speakers` là ba trạng thái.** Gộp mảng rỗng vào "bỏ khoá" sẽ đổi
   nghĩa.
7. **`operatorId` không được lưu.** Nó là người chịu trách nhiệm trong nhật ký
   kiểm toán.
8. **Ràng buộc tên thiết bị.** Bỏ đi sẽ thu nhầm thiết bị sau khi rút USB mic.
9. **Không thêm phụ thuộc protobuf/gRPC.** Cả tầng giao thức viết tay tồn tại là
   để ứng dụng build được chỉ với Qt trên một máy trạm đã triển khai.
10. **`DiagnosticsRunner` không được xoá khi còn chạy.** `runProbe` và
    `runNetworkTests` nhận `const QString&`, còn `run()` truyền thẳng
    `m_target`/`m_token` — tức chúng giữ tham chiếu vào chính object runner suốt
    hàng chục giây. Xoá nó giữa chừng là use-after-free đọc ruột `QString`, âm
    thầm chứ không nổ ra ngay. Vì vậy `onJobCompleted` **không** gọi
    `deleteLater()` (slot đó chạy từ câu lệnh cuối của `run()`, luồng chưa thật
    sự thoát); runner được giữ lại tới lần chạy sau hoặc tới destructor.
