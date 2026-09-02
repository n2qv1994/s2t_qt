# s2t_qt — Hướng dẫn sử dụng

`s2t-qt-client` — ứng dụng khách Qt/C++ cho hệ thống nhận dạng tiếng nói và
phân tách người nói (ASR + diarization). Nó thu microphone trên máy trạm, đẩy
lên **Server buffer** (`s2t-qt-server`) qua gRPC, hiển thị bản chép trực tiếp,
và cho phép soát lại, sửa và lưu vết mọi chỉnh sửa.

Server buffer là chương trình đứng giữa máy trạm và tầng suy luận GPU. Nó giữ
hàng đợi audio, nên một sự cố mạng ở máy trạm hay một lúc tầng suy luận bận
không làm mất tiếng. Người vận hành không cần cấu hình gì cho nó ngoài việc
biết địa chỉ của nó.

Tài liệu này dành cho người vận hành.

| Tài liệu | Dành cho |
|---|---|
| **huong-dan-su-dung.md** (tài liệu này) | người vận hành ứng dụng |
| [luong-hoat-dong.md](luong-hoat-dong.md) | người bảo trì mã nguồn |
| [danh-sach-api.md](danh-sach-api.md) | người tích hợp một client bên ngoài |

---

## 1. Chuẩn bị trước khi dùng

### 1.1 Cấu hình lần đầu

Mở **Công cụ → Cấu hình** (`Ctrl+,`) và điền:

| Mục | Ý nghĩa |
|---|---|
| **Máy chủ đệm (host:port)** | Địa chỉ của `s2t-qt-server`. Mặc định `192.168.1.47:8800`. **Không phải** địa chỉ tầng suy luận (`:8700`) — xem ghi chú ngay dưới bảng. |
| **Bearer token** | Token xác thực của Server buffer. Bấm **Từ tệp...** để đọc từ tệp thay vì gõ tay. Token của tầng suy luận là việc của server và không nằm trên máy này. |
| **Micro** | Thiết bị thu. `(mặc định hệ thống)` để hệ điều hành tự chọn. |
| **Tên thiết bị bắt buộc chứa** | Chuỗi mà tên thiết bị phải chứa, mặc định `Speaker`. |
| **Tần số lấy mẫu / Số kênh** | Mặc định 48000 Hz, 1 kênh. Phải là định dạng thiết bị hỗ trợ. |
| **Hàng đợi tối đa** | Số giây audio đã thu nhưng chưa được server xác nhận, được phép tồn đọng trên máy này. Mặc định 60 s. |
| **xvF3800 host-control** | Đường dẫn tới `xvf_host.exe`, dùng cho nút bật/tắt lọc nhiễu phần cứng. |
| **Bật pipeline trace** | Bật thì mỗi phiên mới lưu vết từng chặng xử lý, xem được ở cửa sổ **Pipeline trace** (`F8`). |
| **Phát lại tệp theo tốc độ thật** | Bật: mô phỏng đúng nhịp cuộc họp (dùng để đo độ trễ). Tắt: xử lý lại một bản ghi càng nhanh càng tốt. |
| **Chế độ nhật ký / Mức nhật ký** | Xem [mục 7](#7-nhật-ký-và-chẩn-đoán). |

Cấu hình được lưu lại và tự nạp ở lần mở sau.

> **Điền nhầm cổng thì sao.** Nếu bạn trỏ vào tầng suy luận (`:8700`) thay vì
> Server buffer (`:8800`), đèn báo sẽ đỏ kèm câu *"…trả lời nhưng không phải
> Server buffer"*. Đó là lỗi cấu hình, không phải lỗi mạng: cả hai đều nói
> gRPC, nhưng chỉ Server buffer mới trả lời được lệnh `ping` của nó. Đừng dùng
> địa chỉ `:8700` — bỏ qua bộ đệm nghĩa là mất toàn bộ khả năng chịu sự cố mạng
> mà nó đem lại.

> **"Tên thiết bị bắt buộc chứa" để làm gì.** Sau khi rút USB mic, Windows có
> thể tái sử dụng đúng endpoint đó cho một thiết bị khác. Nếu không kiểm tra
> tên, ứng dụng sẽ thu nhầm thiết bị mà không báo gì. Có ràng buộc này thì nó
> dừng lại và nói rõ.

### 1.2 Bố cục cửa sổ chính

Cửa sổ được chia theo **ai hỏi câu gì**, không theo loại widget:

| Vùng | Chứa gì |
|---|---|
| **Thanh menu** (trên cùng) | Toàn bộ chức năng, kèm phím tắt. Mọi thứ đều tìm được ở đây, kể cả những cửa sổ không có nút trên thanh công cụ. |
| **Thanh công cụ** | Chỉ những **việc** hay dùng giữa cuộc họp, chia ba nhóm: điều khiển ghi âm · cách hiển thị timeline · hai bảng hay mở. Bên phải là **trạng thái**: tình trạng micro và đèn kết nối. |
| **Giữa** | Timeline bản chép (hoặc chế độ chữ chạy). |
| **Cột phải** | Độ trễ và danh sách cụm từ cần soát lại. |
| **Thanh dưới** | Thông báo, mã phiên, và ô **Người thao tác**. |

Nguyên tắc: thanh **trên** là những gì thay đổi liên tục trong lúc họp; thanh
**dưới** là những gì thay đổi nhiều nhất một lần mỗi cuộc họp.

Phím tắt hay dùng:

| Phím | Việc |
|---|---|
| `Ctrl+R` | Ghi âm từ micro |
| `Ctrl+O` | Chạy tệp audio |
| `Ctrl+P` | Tạm dừng / Tiếp tục |
| `Ctrl+.` | Dừng phiên |
| `Ctrl+L` | Bám trực tiếp theo audio |
| `Ctrl+J` | Nhảy tới chữ mới nhất |
| `Ctrl+T` | Chế độ chữ chạy |
| `Ctrl+W` | Hiện từ độ tin cậy thấp |
| `F5` | Đăng ký giọng nói |
| `F6` | Phụ đề trực tiếp |
| `F7` | Lịch sử hiệu chỉnh |
| `F8` | Pipeline trace |
| `F9` | Bảng soát & sửa |
| `F10` | Nghiệm thu pipeline |
| `F12` | Nhật ký & chẩn đoán |
| `Ctrl+,` | Cấu hình |

### 1.3 Tên người thao tác

Ô **Người thao tác** ở **góc phải thanh dưới** cửa sổ. **Phải nhập lại mỗi lần
mở ứng dụng** — đây là cố ý: tên này được ghi vào nhật ký kiểm toán như người
chịu trách nhiệm cho mỗi lần sửa. Một ô tự điền lại tên người dùng máy lần
trước sẽ ghi công việc của người này dưới tên người khác.

Không có tên người thao tác thì không sửa được văn bản và không đăng ký được
giọng nói.

### 1.4 Kiểm tra kết nối

Đèn báo ở **góc phải thanh công cụ** tự cập nhật mỗi 3 giây. Nó có **ba** trạng thái,
không phải hai, vì giờ có hai chặng mạng và hai chặng ấy hỏng theo hai cách
khác nhau — cần hai cách xử lý khác nhau:

| Đèn | Nghĩa | Cần làm gì |
|---|---|---|
| **● ĐÃ KẾT NỐI AI** (xanh) | Server buffer trả lời, token được chấp nhận, và **nó** tới được tầng suy luận. | Không. |
| **● ĐANG ĐỆM** (vàng) | Server buffer vẫn trả lời bình thường, nhưng nó chưa tới được tầng suy luận. | **Cứ ghi tiếp.** Audio vẫn được nhận và xếp hàng trên server; chữ sẽ hiện ra khi tầng suy luận trở lại. Báo cho người quản trị. |
| **● MẤT KẾT NỐI** (đỏ) | Không tới được Server buffer, hoặc token sai. | Xem [mục 8](#8-xử-lý-sự-cố). Ghi tiếp cũng chỉ dồn hàng đợi trên máy này, và hàng đợi đó có giới hạn. |

Đèn xanh nghĩa là server *trả lời được một RPC thật*, không chỉ là mở được TCP.

Di chuột lên đèn để thấy chi tiết đầy đủ: địa chỉ, phiên bản Server buffer, và
tình trạng tầng suy luận.

---

## 2. Ghi âm một cuộc họp

### 2.1 Bắt đầu

1. Bấm **Ghi âm từ micro** (`Ctrl+R`).
2. Trong hộp thoại, điền phần siêu dữ liệu (đều tùy chọn):
   - **Tên phiên**, **Người tham gia** (cách nhau bằng dấu phẩy).
   - **Mức bảo mật** — chỉ lưu nhãn, *không* thay cho phân quyền phía server.
   - **Chế độ**: `Ghi âm + chuyển văn bản` hoặc `Chỉ ghi âm (không chạy AI)`.
3. Chọn phạm vi nhận diện người nói:
   - **Không giới hạn** — so khớp với toàn bộ database giọng nói chung.
   - **Chỉ những người được chọn** — tick tên trong danh sách bên dưới.
4. Bấm **Bắt đầu ghi âm**.

> **Ba trạng thái, không phải hai.** "Không giới hạn" là so khớp toàn bộ DB.
> "Chỉ những người được chọn" mà **không tick ai** là một chỉ thị khác hẳn:
> *không gán tên đã đăng ký nào cả*. Nó không quay về nghĩa thứ nhất.

Ứng dụng mở microphone **trước**, rồi mới tạo phiên trên server. Nếu thiết bị
không mở được thì bạn nhận lỗi "không ghi được", chứ không phải một phiên rỗng
nằm treo trên server.

### 2.2 Trong lúc ghi

| Nút | Phím | Tác dụng |
|---|---|---|
| **Tạm dừng** | `Ctrl+P` | Ngừng gửi audio, **không** kết thúc phiên. Lời nói trong lúc tạm dừng bị bỏ ngay lúc thu — không được gửi bù sau khi tiếp tục. Bấm lại (nút đổi thành **Tiếp tục**) để chạy tiếp. |
| **Dừng phiên** | `Ctrl+.` | Kết thúc phiên: gửi nốt audio còn trong hàng đợi rồi chốt bản chép. |
| **Bám trực tiếp** | `Ctrl+L` | Timeline tự cuộn theo audio đang tới. Cuộn tay sẽ tự tắt nó, và nút đổi nhãn thành **Đang xem lại**. |
| **Tới chữ mới nhất** | `Ctrl+J` | Nhảy tới từ mới nhất đã nhận được. |
| **Chữ chạy** | `Ctrl+T` | Chuyển sang chế độ một dòng chữ chạy, chữ to. |
| **Hiện từ yếu** | `Ctrl+W` | Mặc định các từ dưới ngưỡng tin cậy bị ẩn. Bật để xem tất cả. |

Lọc nhiễu phần cứng của mic (cần `xvf_host`) nằm ở menu **Micro** — nó được đặt
một lần cho cả buổi chứ không phải thứ bật tắt liên tục, nên không chiếm chỗ
trên thanh công cụ.

**Cột phải** trả lời hai câu hỏi, theo thứ tự:

1. **Độ trễ văn bản** — một con số lớn: chữ đang chậm hơn lời nói bao nhiêu
   giây. Xanh dưới 1.5 s, vàng tới 4 s, đỏ trên đó. Ngay dưới là bốn dòng nói
   thời gian đang trôi đi đâu: tồn đọng tới AI (hàng đợi máy này + hàng đợi
   server), thời gian ACK và chờ AI, tiến độ audio so với đồng hồ, và mốc
   tiếng nói so với mốc văn bản.
2. **Cần soát lại** — các cụm từ mô hình tự đánh giá dưới ngưỡng tin cậy, kèm
   mốc thời gian, người nói và phần trăm. Đây là danh sách việc cần làm ở bảng
   **Soát & sửa**.

Các số phân vị của máy chủ và của máy này, cùng bộ đếm của mô hình bản chép,
nằm sau nút **Chi tiết kỹ thuật** — cần khi báo lỗi, không cần lúc điều hành.

### 2.3 Kết thúc

Bấm **Dừng phiên**. Thanh trạng thái hiện "Đang kết thúc: gửi nốt audio và flush
correction...". Bước này có thể mất từ vài giây đến vài phút — server còn phải
xử lý hết phần audio đã nhận. Đừng đóng ứng dụng trong lúc này.

Xong sẽ có thông báo kèm mã phiên, thời lượng và số revision.

---

## 3. Chạy lại một tệp audio hoặc video

Bấm **Chạy tệp audio** (`Ctrl+O`), khai báo siêu dữ liệu và phạm vi người nói giống mục 2, rồi chọn
tệp. Nhận cả **video**: `.wav .m4a .mp3 .aac .flac .ogg .mp4 .mkv .mov .avi`.

- Tệp không phải WAV PCM 16-bit được giải mã tự động. Nếu máy có `ffmpeg` trên
  PATH thì dùng nó; nếu không thì dùng **FFmpeg đi kèm Qt Multimedia**, nên
  **không cần cài gì thêm trên máy trạm**. Với video, chỉ luồng tiếng được lấy.
- Mọi nguồn đều được đưa về **16 kHz mono** trước khi gửi, vì
  `asr_diar_session` nhận một tensor float không kèm nhịp lấy mẫu và mặc định
  coi mọi thứ là 16 kHz — đưa 48 kHz vào thì bản chép nghe trôi chảy nhưng sai
  hoàn toàn, chứ không báo lỗi.
- Tốc độ phát lại theo tùy chọn **Phát lại tệp theo tốc độ thật** trong Cấu hình.

Tệp đi qua **đúng pipeline** như microphone, nên đây là cách tái hiện một sự cố
mà không cần dựng lại cuộc họp.

---

## 4. Soát lại và sửa bản chép

Bấm **Soát & sửa** (`F9`) để mở bảng soát lại ở đáy cửa sổ.

> **Với cuộc họp dài, đây là nơi duy nhất xem được toàn văn.** Khung bản chép
> đang chạy chỉ giữ **15 phút gần nhất** (và tối đa 180 dòng), để một cuộc họp
> nhiều giờ không làm chương trình phình ra vô hạn. Đoạn trôi quá mốc đó không
> mất — nó nằm đủ trên máy chủ — nhưng phải mở Soát & sửa mới thấy, cuộn khung
> đang chạy thì không ra. Muốn lấy cả cuộc họp thành một tệp thì dùng
> `tools/export_transcript.py`, nó xuất `.txt` và `.docx` đã gộp theo người nói.

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

Menu **Công cụ → Lịch sử hiệu chỉnh** (`F7`) xem toàn bộ nhật ký kiểm toán của phiên: ai sửa gì, lúc nào.

---

## 5. Đăng ký giọng nói

Menu **Phiên → Đăng ký giọng nói** (`F5`). Cần có tên người thao tác.

**Đăng ký giọng mới — thu trực tiếp:**
1. Nhập tên hiển thị.
2. Đọc to đoạn văn mẫu hiện trên màn hình, bấm ghi rồi dừng.
3. Bản ghi được chuyển về 16 kHz mono và gửi lên; server chạy VAD, trích
   embedding và cập nhật DB. Bước này có thể mất tới 2 phút.

**Hoặc nạp từ tệp có sẵn:** bấm **Nạp từ tệp…**. Nhận cả `.wav`, `.mp3`,
`.m4a` lẫn video `.mp4` — phần mềm tự tách tiếng và chuyển về 16 kHz mono, nên
không cần chuẩn bị gì trước.

> **Mẫu tốt quan trọng hơn mẫu dài.** Đoạn phải là **một người nói, liên tục,
> không nhạc nền**. Một tệp có hai người nói sẽ tạo ra một "giọng" pha trộn, và
> sau đó hệ thống gán nhầm tên cho người khác trong các cuộc họp thật — hỏng âm
> thầm, rất khó lần ra. Nếu dùng video quay họp, hãy cắt đúng đoạn một người
> nói trước khi nạp.
>
> Sau khi gửi, hãy đọc dòng kết quả: `speech_seconds_after_vad` cho biết thực
> sự có bao nhiêu giây là tiếng nói. Nếu nó nhỏ hơn nhiều so với độ dài tệp thì
> mẫu có nhiều khoảng lặng — nên thu lại.

> Nếu mạng rớt giữa lúc gửi, **bản ghi vẫn nằm trong cửa sổ** — đừng đóng. Có
> mạng lại thì bấm **Gửi lại bản ghi**.

> Nếu bấm mà báo *"chưa cấu hình dịch vụ đăng ký giọng"*, đó là chuyện của máy
> chủ chứ không phải của bạn: khoá `enroll/url` trong `/etc/s2t-qt-server.conf`
> đang để trống. Báo người quản trị.

**Gán giọng cho một phiên:** ở phần *Người nói trong phiên*, mỗi speaker phát hiện
được trong phiên có thể để nguyên, gán vào một người đã có trong DB, hoặc tạo
mới. Bấm **Lưu lựa chọn**. Từng speaker được báo kết quả riêng — một cái lỗi
không kéo theo các cái còn lại.

---

## 6. Cửa sổ Pipeline trace và Nghiệm thu

**Pipeline trace** (`F8`) — xem từng chặng pipeline đã nhận gì và làm gì với nó, theo số thứ tự
sự kiện. Hai chế độ tách bạch: *realtime* chỉ giữ các thẻ mới nhất (không phình
theo thời gian), *lịch sử* lật ngược về quá khứ theo trang. Nghe lại được audio
thô của từng sự kiện và ghép nhiều span để nghe liền. Cần bật **pipeline trace**
lúc tạo phiên.

**Nghiệm thu pipeline** (`F10`) — bằng chứng để nghiệm thu hệ thống:

- **Mô hình đang dùng** — kiến trúc và version model phía server.
- **Thiết bị & hàng đợi** — lịch sử chuyển trạng thái mic của phiên hiện tại và
  biểu đồ độ trễ: RTT, thời gian chờ AI, phần mạng + gRPC, hàng đợi hai phía.
- **Bằng chứng lọc nhiễu** — ghi ~7 giây đối chứng tắt/bật lọc nhiễu để nghe so
  sánh. Phải dừng phiên mic trước.
- **VAD / Segment**, **CAM++ verify**, **Người nói trong phiên** — tra theo `session_id`.

---

## 7. Nhật ký và chẩn đoán

Menu **Công cụ → Nhật ký & chẩn đoán** (`F12`) mở cửa sổ *Nhật ký & Chẩn đoán*.

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
| **Kiểm tra lại kết nối** | Gọi lại `ping` của Server buffer bằng cấu hình đang chạy — cùng thứ quyết định màu đèn báo (xanh/vàng/đỏ). | Có |
| **Đọc trạng thái đệm** | Gọi `get_buffer_status`: server đã chạy bao lâu, tầng suy luận có sống không, và **mọi phiên đang mở trên server — kể cả của máy khác** với số gói đã nhận, đã đẩy, đang chờ và độ trễ. | Có |
| **Probe máy chủ** | Gọi thật các RPC chỉ-đọc với địa chỉ và token gõ ở trên: model status, danh sách phiên, trạng thái DB giọng nói. Trả lời "server có sống và token có được chấp nhận không" mà **không tạo phiên nào**. | Có |
| **Self-test giao thức** | Kiểm tra bộ mã proto3 và HPACK tự viết bằng các phép round-trip. | Không |
| **Test mạng đầy đủ** | Bộ khẳng định đầu-cuối, cần `tools/mock_adapter.js` đang chạy. Dành cho phát triển; nó dùng mã phiên cố định nên **không chạy được qua Server buffer** — trỏ thẳng vào mock adapter. | Có |

Bấm **Copy báo cáo** để chép kết quả gửi cho người hỗ trợ.

**Bảng trạng thái đệm là chỗ đầu tiên nên nhìn khi chữ ra chậm.** Cột *Trễ* cho
biết tầng suy luận đang chậm hơn thời gian thực bao nhiêu giây; cột *Chờ* là số
gói còn nằm trong hàng đợi. Cả hai lớn dần đều nghĩa là đường ống không theo
kịp — đó là chuyện của người quản trị hệ thống, không phải của máy trạm.

Mỗi lúc chỉ chạy được một phép chẩn đoán; các nút còn lại bị khoá cho tới khi
xong. Nếu máy chủ không phản hồi, phép đang chạy vẫn phải chờ hết deadline của
nó — **Probe** khoảng 12 giây, **Test mạng đầy đủ** có thể lâu hơn nhiều.

### 7.3 Chạy từ dòng lệnh

Các chế độ không cần giao diện:

```
s2t-qt-client --selftest                                 # self-test giao thức
s2t-qt-client --probe 192.168.1.47:8800 --token T        # probe Server buffer
s2t-qt-client --selftest-net 127.0.0.1:18700 --token T   # test mạng đầy đủ
```

Điều khiển log:

```
s2t-qt-client --log-mode develop --log-level trace
s2t-qt-client --log-file D:\loi-hom-nay.log    # tự chuyển sang chế độ develop
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

### "Mất kết nối tới Server buffer"

Phiên được giữ nguyên, audio mới được giữ tạm trên máy này và ứng dụng tự kết
nối lại, gửi lại đúng gói còn dở. Không mất chữ.

Nếu tình trạng kéo dài, hàng đợi trên máy sẽ đầy (theo **Hàng đợi tối đa**) và
phiên **dừng hẳn** thay vì âm thầm xoá audio. Đây là chủ ý: mất tiếng mà không
ai biết thì tệ hơn là dừng lại và báo.

Dùng **Probe máy chủ** ở tab Chẩn đoán để biết là hỏng mạng, sai địa chỉ hay
token bị từ chối.

### Đèn vàng: "ĐANG ĐỆM"

**Cứ ghi tiếp.** Server buffer vẫn nhận audio bình thường và xếp nó vào hàng
đợi; chỉ có tầng suy luận GPU là chưa tới được, nên chữ tạm thời không ra. Khi
tầng suy luận trở lại, toàn bộ phần đã xếp hàng được đẩy lên theo đúng thứ tự
và bản chép bắt kịp.

Bấm **Đọc trạng thái đệm** ở tab Chẩn đoán để xem hàng đợi đang lớn tới đâu, và
báo cho người quản trị hệ thống. Đây là sự cố phía máy chủ, không phải phía máy
trạm.

Bộ đệm cũng có giới hạn (mặc định 300 giây audio mỗi phiên). Nếu tầng suy luận
không trở lại trước khi hết chỗ, phiên sẽ dừng ồn ào với thông báo *"bộ đệm
phiên đã đầy"* — lại là chủ ý, cùng lý do như trên.

### "Máy chủ đệm không giữ phiên …"

Đọc kỹ phần trong ngoặc của thông báo, vì có hai nguyên nhân khác hẳn nhau:

- **"…nhật ký phiên đang TẮT…"** — Server buffer đã khởi động lại giữa chừng và
  nó đang chạy **không** có nhật ký, nên hàng đợi chỉ nằm trong bộ nhớ. Báo cho
  người quản trị: đặt `buffer/journal_dir` là hết hẳn tình trạng này.
- **"…phiên đã kết thúc, hoặc đã quá hạn giữ…"** — phiên đã dừng bình thường từ
  trước, hoặc đã dừng quá lâu và bị quên đi. Không phải sự cố.

Trong cả hai trường hợp, bản chép **không mất**: tầng suy luận vẫn giữ phiên
ấy, nên nó vẫn nằm trong danh sách ở màn hình **SOÁT LẠI** và vẫn sửa được. Chỉ
việc ghi tiếp vào phiên cũ là không được — bắt đầu một phiên mới.

### Server buffer khởi động lại giữa lúc đang ghi

Nếu người quản trị đã bật nhật ký phiên (`buffer/journal_dir`), **không cần làm
gì cả**. Đèn báo đỏ vài giây, ứng dụng tự kết nối lại và gửi lại đúng phần còn
dở, rồi cuộc họp đi tiếp. Không mất chữ.

Chỉ khi lần khởi động lại kéo dài hơn **Hàng đợi tối đa** (mặc định 60 giây)
thì hàng đợi trên máy này mới đầy và phiên dừng ồn ào — vẫn là dừng có báo, chứ
không phải mất tiếng âm thầm.

Nếu nhật ký đang tắt thì phiên không sống sót; xem mục ngay trên.

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
- **Phiên không tự kết thúc khi mất mạng hay mất mic.** Chỉ có nút **Dừng phiên**, lỗi
  không khắc phục được, hoặc đóng ứng dụng mới kết thúc phiên.
- **Đóng ứng dụng khi đang ghi sẽ cắt phiên** mà không flush correction. Ứng
  dụng có hỏi lại trước khi làm.
- **Tên người thao tác không được nhớ giữa các lần chạy** — xem mục 1.2.
- **Mức bảo mật chỉ là nhãn**, không thay cho phân quyền phía server.
- **Ứng dụng này không phải là server.** Nó không mở cổng nào và không có API
  để phần mềm khác gọi vào. Phần server là chương trình riêng — `s2t-qt-server`
  — và nó *có* API để phần mềm khác gọi vào; xem
  [danh-sach-api.md](danh-sach-api.md).
- **Một phiên chỉ nên có một client đang ghi**, nhưng **nhiều client xem chung
  một phiên thì được**. Hai chương trình cùng đẩy audio vào một `session_id` sẽ
  làm hỏng bản chép, vì bộ đếm gói của mỗi bên là độc lập với bên kia. Ngược
  lại, việc nhiều người cùng theo dõi một cuộc họp là rẻ: Server buffer đệm
  trạng thái trong 200 ms, nên mười người xem chỉ tốn của tầng suy luận đúng
  một lần đọc.
- **Máy trạm chỉ cần mở một cổng ra ngoài**, tới Server buffer. Địa chỉ và
  token của tầng suy luận GPU không còn nằm trên máy của người vận hành.
- **Phiên có sống qua lần khởi động lại của Server buffer hay không là do cấu
  hình phía server** (`buffer/journal_dir`). Bật thì một lần khởi động lại gần
  như vô hình với người đang ghi; tắt thì mất cuộc họp đang mở. Thông báo lỗi
  nói rõ đang ở trường hợp nào — xem mục 8.

---

## 10. Cho bộ phận tích hợp

Nếu đơn vị bạn cần đưa dịch vụ này vào một phần mềm khác (tổng đài, hệ thống
lưu trữ cuộc họp, quy trình xử lý hàng loạt), thì thứ cần đọc là
[danh-sach-api.md](danh-sach-api.md). Tài liệu đó liệt kê toàn bộ 20 RPC, định
dạng audio, chính sách thử lại và một tệp `.proto` sẵn sàng biên dịch.

Client mới nên nối vào **Server buffer**, không phải vào tầng suy luận: đó là
nơi có hàng đợi audio, và nó nói đúng hợp đồng mà tầng suy luận nói, cộng thêm
ba RPC quản trị của riêng nó.

Hai thông tin người vận hành cần cấp cho bộ phận tích hợp, lấy ngay trong
**Cấu hình** của ứng dụng này:

| Cần | Lấy ở đâu |
|---|---|
| Địa chỉ Server buffer (`host:port`) | ô **Server buffer** |
| Bearer token | ô **Bearer token** |

Token là bí mật: gửi qua kênh nội bộ, đừng dán vào tài liệu dùng chung, và
đừng để nó lọt vào log. Nếu nghi ngờ lộ, đề nghị quản trị cấp lại — đổi token
không ảnh hưởng tới các phiên đã lưu.
