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
│      s2t-qt-client        │   │     s2t-qt-server      │   │ grpc_session_    │
│  microphone, giao diện,   │──►│  hàng đợi audio,       │──►│ adapter.py :8700 │──► Triton
│  soát/sửa, đăng ký giọng  │   │  chuyển tiếp, bộ đệm   │   │                  │
│                           │◄──│  trạng thái            │◄──│                  │◄──
└───────────────────────────┘   └────────────────────────┘   └──────────────────┘
        gRPC :8800                      gRPC :8700
   3 service, 20 RPC, 1 token      2 service, 17 RPC, 1 token
```

**Ranh giới đặt ở đâu, và vì sao ở đó.** Bốn RPC do bộ đệm *trả lời*:
`start_session`, `push_audio`, `get_live_state`, `stop_session` — đúng những
RPC chạm vào đường audio hoặc trạng thái của nó. Mười sáu RPC còn lại được
**chuyển tiếp nguyên vẹn**: bộ đệm không có ý kiến gì về một truy vấn soát lại,
và nếu nó tự nghĩ ra một ý kiến thì sẽ có hai nơi có thể bất đồng về nội dung
một bản chép.

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
     │  4 RPC có đệm    │   │   16 RPC chuyển tiếp│   │   3 RPC quản trị    │
     └────────┬─────────┘   └──────────┬──────────┘   └─────────────────────┘
              │                        │
   ┌──────────▼───────────┐   ┌────────▼─────────────────────────┐
   │ SessionBuffer (mỗi   │   │ UpstreamPool: relay-lane-0..N     │
   │ phiên một cái)       │   │ mỗi lane một RpcLane = 1 luồng    │
   │  ├ forward-<phiên>   │   │ + 1 kênh                          │
   │  │   hàng đợi + đẩy  │   └────────┬─────────────────────────┘
   │  └ state-<phiên>     │            │
   │      bộ đệm 200 ms   │            │        ┌──────────────────┐
   └──────────┬───────────┘            │        │ upstream-probe   │
              └────────────────────────┴────────┤ ping mỗi 5 giây  │
                                       │        └──────────────────┘
                          ┌────────────▼────────────┐
                          │ AsrClient → grpc::Channel│
                          └────────────┬────────────┘
                                       │  h2c, bearer token của adapter
                                 tầng suy luận :8700
```

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
| `relay-lane-0..N` | `UpstreamPool` | Mọi RPC chuyển tiếp. Mặc định 4 lane. |
| `upstream-probe` | `BufferHub` | `ping` tầng suy luận mỗi 5 giây, và ngay lập tức sau khi một relay thất bại. Kênh riêng, vì một phép thử xếp sau bốn lane đang bận sẽ báo về một sự đình trệ của *chúng ta* chứ không phải của tầng suy luận. |

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
  ├───────────────────────────►│  qua UpstreamPool         │                      │
  │                            ├──────────────────────────────────────────────────►│
  │                            │◄──────────────── session_id + state đầu tiên ─────┤
  │                            │  tạo SessionBuffer ──────►│  (2 luồng, 2 kênh)
  │◄─────── session_id ────────┤                           ├──── (chưa gửi gì) ───►│
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

### 7b.7 Phiên không sống qua lần khởi động lại

Hàng đợi nằm trong RAM và bản đồ phiên cũng vậy. Sau khi server khởi động lại,
`push_audio` cho một phiên cũ trả `NOT_FOUND`, và thông báo nói thẳng rằng phiên
bắt đầu trước lần khởi động lại thì không còn ở đây. Tầng suy luận vẫn còn phiên
ấy — chỉ bộ đệm là không — nên bản chép vẫn soát lại được qua `get_review_state`.

Chuyển tiếp mù `push_audio` cho một phiên không có bộ đệm sẽ làm mọi bộ đếm nói
dối, nên nó không được phép.

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

## 13. Bản đồ mã nguồn

| Thư mục | Nội dung |
|---|---|
| `shared/proto/` | `ProtoWire` (proto3 tay, hai chiều), `AsrSession`, `SpeakerRegistry`, `BufferAdmin` — struct chép tay theo `.proto` |
| `shared/grpc/` | `Hpack`, `Http2Client`, `GrpcChannel`, `AsrClient` (gọi đi); `Http2Server`, `GrpcServer` (nhận); `Methods.h` (đường method dùng chung cho cả hai bên) |
| `shared/core/` | `Logger` — hệ thống nhật ký, chung cho cả hai chương trình |
| `s2t-qt-server/` | `main`, `ServerConfig`, `RpcLane`, `UpstreamPool`, `SessionBuffer`, `BufferHub`, `BufferService`, `ServerSelfTest` |
| `s2t-qt-client/core/` | `SessionController` (mặt tiền), `SessionWorker`, `StatePoller`, `RpcExecutor`, `AudioQueue`, `TranscriptModel`, `AppConfig`, `SelfTest` |
| `s2t-qt-client/audio/` | `AudioCapture`, `MicDenoise` (điều khiển xvf3800), `WavIo`, `Transcode` (ffmpeg) |
| `s2t-qt-client/ui/` | `TimelineView`, `Dialogs`, `ReviewPanel`, `EnrollDialog`, `TraceWindow`, `EvidenceWindow`, `DiagnosticsWindow`, `LogControls` (combo chế độ/mức log dùng chung) |
| `tools/` | `mock_adapter.js` (peer HTTP/2 độc lập cho `--selftest-net`), `build_rhel9.sh`, `deploy_rhel.sh` (đẩy mã nguồn + build + `--selftest` lên máy RHEL), `run_valgrind.sh`, `valgrind.supp`, `s2t-qt-server.service`, `s2t-qt-server.conf.sample` |
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
s2t-qt-server --probe 192.168.1.47:8700 --token T    # tầng suy luận có sống không

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
thứ tự gói, rào chắn drain, phát lại ACK, và bộ đệm trạng thái.

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
