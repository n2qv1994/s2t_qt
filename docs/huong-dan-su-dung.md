# s2t_qt — Hướng dẫn sử dụng

Ứng dụng khách Qt/C++ cho hệ thống nhận dạng tiếng nói và phân tách người nói
(ASR + diarization). Ứng dụng thu microphone trên máy trạm, đẩy lên AI server
qua gRPC, hiển thị bản chép trực tiếp, và cho phép soát lại, sửa và lưu vết
mọi chỉnh sửa.

Tài liệu này dành cho người vận hành.

| Tài liệu | Dành cho |
|---|---|
| **huong-dan-su-dung.md** (tài liệu này) | người vận hành ứng dụng |
| [luong-hoat-dong.md](luong-hoat-dong.md) | người bảo trì mã nguồn |
| [danh-sach-api.md](danh-sach-api.md) | người tích hợp một client bên ngoài |

---

## 1. Chuẩn bị trước khi dùng

### 1.1 Cấu hình lần đầu

Mở **Cấu hình** trên thanh công cụ và điền:

| Mục | Ý nghĩa |
|---|---|
| **AI server (host:port)** | Địa chỉ bộ chuyển tiếp gRPC. Mặc định `192.168.1.47:8700`. |
| **Bearer token** | Token xác thực. Bấm **Từ tệp...** để đọc từ tệp thay vì gõ tay. |
| **Microphone** | Thiết bị thu. `(mặc định hệ thống)` để hệ điều hành tự chọn. |
| **Tên thiết bị bắt buộc chứa** | Chuỗi mà tên thiết bị phải chứa, mặc định `Speaker`. |
| **Sample rate / Số kênh** | Mặc định 48000 Hz, 1 kênh. Phải là định dạng thiết bị hỗ trợ. |
| **Hàng đợi tối đa** | Số giây audio đã thu nhưng chưa được server xác nhận, được phép tồn đọng trên máy này. Mặc định 60 s. |
| **xvF3800 host-control** | Đường dẫn tới `xvf_host.exe`, dùng cho nút bật/tắt lọc nhiễu phần cứng. |
| **Bật pipeline trace** | Bật thì mỗi phiên mới lưu vết từng chặng xử lý, xem được ở cửa sổ TRACE. |
| **Phát lại tệp theo tốc độ thật** | Bật: mô phỏng đúng nhịp cuộc họp (dùng để đo độ trễ). Tắt: xử lý lại một bản ghi càng nhanh càng tốt. |
| **Chế độ log / Mức log** | Xem [mục 7](#7-nhật-ký-và-chẩn-đoán). |

Cấu hình được lưu lại và tự nạp ở lần mở sau.

> **"Tên thiết bị bắt buộc chứa" để làm gì.** Sau khi rút USB mic, Windows có
> thể tái sử dụng đúng endpoint đó cho một thiết bị khác. Nếu không kiểm tra
> tên, ứng dụng sẽ thu nhầm thiết bị mà không báo gì. Có ràng buộc này thì nó
> dừng lại và nói rõ.

### 1.2 Tên người thao tác

Ô **Người thao tác** ở góc phải thanh công cụ. **Phải nhập lại mỗi lần mở ứng
dụng** — đây là cố ý: tên này được ghi vào nhật ký kiểm toán như người chịu
trách nhiệm cho mỗi lần sửa. Một ô tự điền lại tên người dùng máy lần trước sẽ
ghi công việc của người này dưới tên người khác.

Không có tên người thao tác thì không sửa được văn bản và không đăng ký được
giọng nói.

### 1.3 Kiểm tra kết nối

Đèn báo ở đầu thanh công cụ tự cập nhật mỗi 3 giây:

- **● AI ĐÃ KẾT NỐI** — server trả lời và token được chấp nhận.
- **● MẤT KẾT NỐI AI** — kèm lý do. Xem [mục 8](#8-xử-lý-sự-cố).

Đèn xanh nghĩa là server *trả lời được một RPC thật*, không chỉ là mở được TCP.

---

## 2. Ghi âm một cuộc họp

### 2.1 Bắt đầu

1. Bấm **BẮT ĐẦU GHI ÂM**.
2. Trong hộp thoại, điền phần siêu dữ liệu (đều tùy chọn):
   - **Tên phiên**, **Người tham gia** (cách nhau bằng dấu phẩy).
   - **Mức bảo mật** — chỉ lưu nhãn, *không* thay cho phân quyền phía server.
   - **Chế độ**: `Ghi âm + chuyển văn bản` hoặc `Chỉ ghi âm (không chạy AI)`.
3. Chọn phạm vi nhận diện người nói:
   - **Không giới hạn** — so khớp với toàn bộ database giọng nói chung.
   - **Chỉ những người được chọn** — tick tên trong danh sách bên dưới.
4. Bấm **BẮT ĐẦU GHI ÂM**.

> **Ba trạng thái, không phải hai.** "Không giới hạn" là so khớp toàn bộ DB.
> "Chỉ những người được chọn" mà **không tick ai** là một chỉ thị khác hẳn:
> *không gán tên đã đăng ký nào cả*. Nó không quay về nghĩa thứ nhất.

Ứng dụng mở microphone **trước**, rồi mới tạo phiên trên server. Nếu thiết bị
không mở được thì bạn nhận lỗi "không ghi được", chứ không phải một phiên rỗng
nằm treo trên server.

### 2.2 Trong lúc ghi

| Nút | Tác dụng |
|---|---|
| **PAUSE** | Ngừng gửi audio, **không** kết thúc phiên. Lời nói trong lúc tạm dừng bị bỏ ngay lúc thu — không được gửi bù sau khi tiếp tục. |
| **STOP** | Kết thúc phiên: gửi nốt audio còn trong hàng đợi rồi chốt bản chép. |
| **DENOISE ON / OFF** | Bật/tắt lọc nhiễu phần cứng trên mic (cần `xvf_host`). |
| **TEXT** | Nhảy tới chữ mới nhất. |
| **LIVE** | Bật/tắt tự bám theo audio đang chạy. Cuộn tay sẽ tự tắt nó. |
| **TICKER** | Chuyển sang chế độ một dòng chữ chạy, chữ to. |
| **HIỆN TỪ YẾU** | Mặc định các từ dưới ngưỡng tin cậy bị ẩn. Bật để xem tất cả. |

Bảng bên phải hiển thị **Highlights** (các đoạn dưới 75% tin cậy) và một ô
`delay` với số liệu độ trễ theo thời gian thực.

### 2.3 Kết thúc

Bấm **STOP**. Thanh trạng thái hiện "Đang kết thúc: gửi nốt audio và flush
correction...". Bước này có thể mất từ vài giây đến vài phút — server còn phải
xử lý hết phần audio đã nhận. Đừng đóng ứng dụng trong lúc này.

Xong sẽ có thông báo kèm mã phiên, thời lượng và số revision.

---

## 3. Chạy lại một tệp audio

Bấm **AUDIO**, khai báo siêu dữ liệu và phạm vi người nói giống mục 2, rồi chọn
tệp `.wav` hoặc `.m4a`.

- `.m4a` và `.wav` không phải PCM 16-bit sẽ được chuyển mã qua **ffmpeg** trước
  (cần có `ffmpeg` trên PATH).
- Tốc độ phát lại theo tùy chọn **Phát lại tệp theo tốc độ thật** trong Cấu hình.

Tệp đi qua **đúng pipeline** như microphone, nên đây là cách tái hiện một sự cố
mà không cần dựng lại cuộc họp.

---

## 4. Soát lại và sửa bản chép

Bấm **REVIEW** để mở bảng soát lại ở đáy cửa sổ.

- **Tải danh sách** rồi chọn phiên; hoặc gõ thẳng `session_id`.
- **Sửa một câu**: bấm đúp vào ô văn bản, sửa, xác nhận.
- **Đổi tên người nói**: bấm đúp vào ô người nói. Có thể đặt tên hiển thị hoặc
  gộp vào một speaker khác.
- **Nghe lại**: bấm đúp vào ô thời gian để phát đúng đoạn audio quanh đó.

Mỗi lần sửa đều mang theo `base_revision`. Nếu server đã nhích revision từ lúc
bạn tải về, chỉnh sửa sẽ bị từ chối và bảng tự tải lại — không có chuyện ghi đè
âm thầm lên việc của người khác.

Nếu nhận được *"chưa chốt tới đoạn này, thử lại sau vài giây"*: đoạn đó vẫn còn
là kết quả tạm, chưa chốt. Đợi vài giây rồi sửa lại.

Bấm **LỊCH SỬ** để xem toàn bộ nhật ký kiểm toán của phiên: ai sửa gì, lúc nào.

---

## 5. Đăng ký giọng nói

Bấm **SETUP**. Cần có tên người thao tác.

**Đăng ký giọng mới:**
1. Nhập tên hiển thị.
2. Đọc to đoạn văn mẫu hiện trên màn hình, bấm ghi rồi dừng.
3. Bản ghi được chuyển về 16 kHz mono và gửi lên; server chạy VAD, trích
   embedding và cập nhật DB. Bước này có thể mất tới 2 phút.

> Nếu mạng rớt giữa lúc gửi, **bản ghi vẫn nằm trong cửa sổ** — đừng đóng. Có
> mạng lại thì bấm **Gửi lại bản ghi**.

**Gán giọng cho một phiên:** ở phần *Speaker của phiên*, mỗi speaker phát hiện
được trong phiên có thể để nguyên, gán vào một người đã có trong DB, hoặc tạo
mới. Bấm **Lưu lựa chọn**. Từng speaker được báo kết quả riêng — một cái lỗi
không kéo theo các cái còn lại.

---

## 6. Cửa sổ TRACE và NGHIỆM THU

**TRACE** — xem từng chặng pipeline đã nhận gì và làm gì với nó, theo số thứ tự
sự kiện. Hai chế độ tách bạch: *realtime* chỉ giữ các thẻ mới nhất (không phình
theo thời gian), *lịch sử* lật ngược về quá khứ theo trang. Nghe lại được audio
thô của từng sự kiện và ghép nhiều span để nghe liền. Cần bật **pipeline trace**
lúc tạo phiên.

**NGHIỆM THU** — bằng chứng để nghiệm thu hệ thống:

- **Mô hình đang dùng** — kiến trúc và version model phía server.
- **Thiết bị & hàng đợi** — lịch sử chuyển trạng thái mic của phiên hiện tại và
  biểu đồ độ trễ: RTT, thời gian chờ AI, phần mạng + gRPC, hàng đợi hai phía.
- **Bằng chứng lọc nhiễu** — ghi ~7 giây đối chứng tắt/bật lọc nhiễu để nghe so
  sánh. Phải dừng phiên mic trước.
- **VAD / Segment**, **CAM++ verify**, **Speaker của phiên** — tra theo `session_id`.

---

## 7. Nhật ký và chẩn đoán

Bấm **NHẬT KÝ** để mở cửa sổ *Nhật ký & Chẩn đoán*.

### 7.1 Tab Nhật ký

Xem trực tiếp mọi việc ứng dụng đang làm.

- **Chế độ** — `Debug` in ra console (chỉ thấy nếu mở ứng dụng từ cửa sổ lệnh);
  `Develop` ghi ra tệp, luôn đọc lại được. Đổi là áp dụng ngay và được ghi nhớ.
- **Mức ghi** — quyết định dòng nào được ghi:

  | Mức | Dùng khi |
  |---|---|
  | `trace` | Đang tái hiện lỗi. Ghi từng gói audio — rất nhiều. |
  | `debug` | Mặc định. Đủ để lần theo luồng hoạt động. |
  | `info` | Chỉ các mốc chính. |
  | `warn` / `error` | Chỉ cảnh báo / chỉ lỗi. |

- **Hiện từ mức**, **Thành phần**, **Tìm** — lọc phần đang xem (không ảnh hưởng
  cái được ghi). Thành phần là các nhóm `session`, `worker`, `audio`, `grpc`,
  `http2`, `poll`, `rpc`, `ui`, `config`, `queue`, `model`, `app`, `qt`.
- **Tự cuộn** / **Tạm giữ** — tạm giữ để đọc yên; log vẫn được ghi bình thường.
- **Lưu ra tệp...** — lưu đúng phần đang hiện theo bộ lọc, tiện gửi đi.
- **Mở thư mục log** — mở thư mục chứa tệp log.

Tệp log nằm ở `%APPDATA%\s2t\s2t_qt\logs\s2t_qt.log`, tự xoay vòng khi đầy 8 MB
và giữ 2 đời cũ. Mỗi dòng được ghi xuống đĩa ngay, nên vẫn còn sau khi sập
nguồn.

Định dạng một dòng:

```
2026-08-22 10:42:38.455 WARN  grpc  rpc-lane-0  GrpcChannel.cpp:80 | /asr.ui.v1...
   thời gian          mức  thành phần  luồng      vị trí trong mã   nội dung
```

### 7.2 Tab Chẩn đoán

| Nút | Làm gì | Cần mạng |
|---|---|---|
| **Kiểm tra lại kết nối** | Gọi lại `get_model_status` bằng cấu hình đang chạy — cùng thứ quyết định màu đèn báo. | Có |
| **Probe máy chủ** | Gọi thật các RPC chỉ-đọc với địa chỉ và token gõ ở trên: model status, danh sách phiên, trạng thái DB giọng nói. Trả lời "server có sống và token có được chấp nhận không" mà **không tạo phiên nào**. | Có |
| **Self-test giao thức** | Kiểm tra bộ mã proto3 và HPACK tự viết bằng các phép round-trip. | Không |
| **Test mạng đầy đủ** | Bộ khẳng định đầu-cuối, cần `tools/mock_adapter.js` đang chạy. Dành cho phát triển. | Có |

Bấm **Copy báo cáo** để chép kết quả gửi cho người hỗ trợ.

Mỗi lúc chỉ chạy được một phép chẩn đoán; các nút còn lại bị khoá cho tới khi
xong. Nếu máy chủ không phản hồi, phép đang chạy vẫn phải chờ hết deadline của
nó — **Probe** khoảng 12 giây, **Test mạng đầy đủ** có thể lâu hơn nhiều.

### 7.3 Chạy từ dòng lệnh

Các chế độ không cần giao diện:

```
s2t_qt --selftest                              # self-test giao thức
s2t_qt --probe 192.168.1.47:8700 --token T     # probe máy chủ
s2t_qt --selftest-net 127.0.0.1:8700 --token T # test mạng đầy đủ
```

Điều khiển log:

```
s2t_qt --log-mode develop --log-level trace
s2t_qt --log-file D:\loi-hom-nay.log           # tự chuyển sang chế độ develop
```

Hoặc bằng biến môi trường `S2T_LOG_MODE`, `S2T_LOG_LEVEL`, `S2T_LOG_FILE`.
Thứ tự ưu tiên: dòng lệnh → biến môi trường → cấu hình đã lưu → mặc định lúc
build. Riêng việc đổi trong ứng dụng thì luôn thắng, vì đó là người đang yêu cầu
ngay lúc đó.

---

## 8. Xử lý sự cố

### "Microphone bị ngắt khi đang ghi"

Phiên **không** kết thúc — nó chuyển sang trạng thái chờ và thử mở lại thiết bị
hai lần mỗi giây. Cắm lại mic là ghi tiếp cùng một phiên.

Phần audio trong lúc mic mất thì không lấy lại được (nó chưa từng được thu),
nhưng phần trước và sau vẫn thuộc cùng một cuộc họp.

### "Thiết bị đã cấu hình không còn đúng microphone"

Endpoint vẫn còn nhưng tên đã đổi — thường là do rút USB mic rồi cắm thiết bị
khác. Kiểm tra lại **Microphone** và **Tên thiết bị bắt buộc chứa** trong Cấu
hình.

### "Mất kết nối tới AI server"

Phiên được giữ nguyên, audio mới được giữ tạm trên máy này và ứng dụng tự kết
nối lại, gửi lại đúng gói còn dở. Không mất chữ.

Nếu tình trạng kéo dài, hàng đợi trên máy sẽ đầy (theo **Hàng đợi tối đa**) và
phiên **dừng hẳn** thay vì âm thầm xoá audio. Đây là chủ ý: mất tiếng mà không
ai biết thì tệ hơn là dừng lại và báo.

Dùng **Probe máy chủ** ở tab Chẩn đoán để biết là hỏng mạng, sai địa chỉ hay
token bị từ chối.

### "Hàng đợi microphone bị tràn"

Máy này thu nhanh hơn mức server nhận được, kéo dài quá ngưỡng đã đặt. Kiểm tra
mạng và tải của server; có thể tăng **Hàng đợi tối đa** nếu đường truyền hay
chập chờn theo đợt.

### Không mở được microphone

Kiểm tra theo thứ tự: USB mic còn cắm không, có đúng là thiết bị đầu vào đang
chọn không, có tiến trình khác đang giữ thiết bị không. Sửa xong thì bắt đầu
phiên mới.

### Không thấy log ở đâu cả

Nhiều khả năng đang ở chế độ `Debug` mà ứng dụng lại được mở bằng nhấp đúp —
không có console nào để in ra. Đổi sang **Develop** trong tab Nhật ký.

### Đóng ứng dụng mà nó đứng im một lúc rồi mới tắt

Nếu lúc đó còn một phép chẩn đoán đang chạy và máy chủ không phản hồi, ứng dụng
chờ nó tối đa 20 giây rồi mới thoát — cố ý, để không cắt ngang giữa chừng. Không
cần làm gì, cứ đợi.

### Ứng dụng báo cần màn hình đồ hoạ (trên Linux)

Phiên ssh không có `DISPLAY`/`WAYLAND_DISPLAY`. Dùng `ssh -X`, hoặc chạy trên
console/VNC của máy, hoặc dùng các chế độ dòng lệnh ở mục 7.3.

---

## 9. Những điều nên biết

- **Audio lúc tạm dừng không bao giờ được gửi bù.** Bỏ ngay lúc thu, không phải
  lúc gửi.
- **Phiên không tự kết thúc khi mất mạng hay mất mic.** Chỉ có nút STOP, lỗi
  không khắc phục được, hoặc đóng ứng dụng mới kết thúc phiên.
- **Đóng ứng dụng khi đang ghi sẽ cắt phiên** mà không flush correction. Ứng
  dụng có hỏi lại trước khi làm.
- **Tên người thao tác không được nhớ giữa các lần chạy** — xem mục 1.2.
- **Mức bảo mật chỉ là nhãn**, không thay cho phân quyền phía server.
- **Ứng dụng này không phải là server.** Nó không mở cổng nào và không có API
  để phần mềm khác gọi vào. Muốn một hệ thống khác dùng dịch vụ xử lý âm thanh
  thì hệ thống đó nói chuyện thẳng với AI server bằng đúng hợp đồng mà ứng dụng
  này đang dùng — xem [danh-sach-api.md](danh-sach-api.md).
- **Một phiên chỉ nên có một client đang ghi.** Hai chương trình cùng đẩy audio
  vào một `session_id` sẽ làm hỏng bản chép, vì bộ đếm gói của mỗi bên là độc
  lập với bên kia.

---

## 10. Cho bộ phận tích hợp

Nếu đơn vị bạn cần đưa dịch vụ này vào một phần mềm khác (tổng đài, hệ thống
lưu trữ cuộc họp, quy trình xử lý hàng loạt), thì thứ cần đọc là
[danh-sach-api.md](danh-sach-api.md). Tài liệu đó liệt kê toàn bộ 17 RPC, định
dạng audio, chính sách thử lại và một tệp `.proto` sẵn sàng biên dịch.

Hai thông tin người vận hành cần cấp cho bộ phận tích hợp, lấy ngay trong
**Cấu hình** của ứng dụng này:

| Cần | Lấy ở đâu |
|---|---|
| Địa chỉ AI server (`host:port`) | ô **AI server** |
| Bearer token | ô **Bearer token** |

Token là bí mật: gửi qua kênh nội bộ, đừng dán vào tài liệu dùng chung, và
đừng để nó lọt vào log. Nếu nghi ngờ lộ, đề nghị quản trị AI server cấp lại —
đổi token không ảnh hưởng tới các phiên đã lưu.
