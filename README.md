# s2t_qt — Server buffer + client Qt/C++ cho hệ thống S2T

Hai chương trình Qt 6, một cây nguồn:

| Dự án | Là gì | Cần gì |
|---|---|---|
| **`s2t-qt-server`** | *Server buffer* trong sơ đồ luồng tổng thể (`docs/slide.pdf`). Nhận audio từ các client qua gRPC, đệm lại, rồi đẩy lên tầng suy luận. Không có giao diện. | Qt Core + Network |
| **`s2t-qt-client`** | Giao diện điều hành viên: thu microphone, xem transcript trực tiếp, soát/sửa, đăng ký giọng. Chỉ nói chuyện với Server buffer. | Qt Widgets + Multimedia + Network |

```
Windows/RHEL client        Server buffer                  tầng suy luận
┌────────────────┐        ┌──────────────┐        ┌────────────────────────┐
│ s2t-qt-client  │ gRPC   │ s2t-qt-server│ gRPC   │ Triton  :8011  KServe v2│
│ mic, UI, sửa   │──────► │ hàng đợi     │──────► │   asr_diar_session      │
│                │ :8800  │ + bản chép   │        │        ── hoặc ──       │
└────────────────┘        └──────────────┘        │ Riva   :50051  nvidia   │
                                                  │   .riva.asr             │
                                                  └────────────────────────┘
```

**Từ 2026-08-25, máy chủ nói thẳng với tầng suy luận.**
`grpc_session_adapter.py` không còn trong sơ đồ triển khai: nó vốn là mã mẫu
để hiểu nghiệp vụ. `s2t-qt-server/backend/InferenceBackend.h` là ranh giới, và
`upstream/backend` trong tệp cấu hình chọn `triton` hay `riva`. Không có gì
phía trên ranh giới đó biết mình đang nói với bên nào.

Hệ quả lớn nhất: **bản chép giờ được dựng ở máy chủ này**, trong
`LiveTranscript`. Riva và Triton đều trả lời theo từng gói và không nhớ gì cả,
nên không còn ai ở trên để hỏi `get_live_state` nữa.

Trước đây `s2t_qt` là một chương trình duy nhất nối thẳng lên adapter. Việc
tách đôi đặt hàng đợi audio ra khỏi máy trạm, và đó là toàn bộ lý do:

- **Sự cố mạng ở máy trạm không còn làm mất tiếng.** Client được trả lời ngay
  khi gói audio nằm chắc chắn trong bộ đệm của server; từ đó trở đi việc thử
  lại là chuyện của server, ở một chặng gần tầng suy luận hơn.
- **Khởi động lại máy chủ đệm không làm hỏng cuộc họp.** Bật
  `buffer/journal_dir` thì mỗi gói được ghi xuống đĩa *trước* khi client được
  ACK; lần khởi động sau đọc lại, đẩy nốt phần chưa gửi, và client ghi tiếp như
  chưa có gì xảy ra.
- **Tầng suy luận sập không làm dừng cuộc họp.** Audio vẫn được nhận và xếp
  hàng (mặc định 300 giây mỗi phiên). Đèn báo trên client chuyển vàng —
  *đang đệm* — chứ không đỏ.
- **Nhiều người xem một cuộc họp không tốn gì của tầng suy luận.** Bản chép
  nằm sẵn ở máy chủ này, nên `get_live_state` là một khoá và một bản sao -
  mười client cùng theo dõi vẫn là *không* lần gọi nào lên tầng suy luận.
- **Máy trạm chỉ cần mở đúng một cổng ra ngoài**, tới Server buffer. Địa chỉ
  và token của tầng suy luận không còn nằm trên máy của điều hành viên.

Kho mô hình Triton không đổi một dòng nào — `asr_diar_session` và mười mô hình
phía sau nó vẫn nguyên. Thứ biến mất là lớp Python đứng trước chúng.

## Tài liệu

Ba tài liệu tiếng Việt trong `docs/`:

| Tài liệu | Cho ai | Nội dung |
|---|---|---|
| [`docs/huong-dan-su-dung.md`](docs/huong-dan-su-dung.md) | điều hành viên | cấu hình, ghi cuộc họp, soát/sửa, đăng ký giọng, xử lý sự cố |
| [`docs/luong-hoat-dong.md`](docs/luong-hoat-dong.md) | người bảo trì mã nguồn | luồng, kênh, vòng đời phiên qua cả hai tiến trình, tầng giao thức, danh sách ràng buộc không được phá |
| [`docs/danh-sach-api.md`](docs/danh-sach-api.md) | **người viết client khác** | cả 20 RPC với từng số hiệu trường, chính sách lỗi/thử lại, định dạng audio, `config_json`, ví dụ Python và C++, và tệp `.proto` tái dựng |

`docs/slide.pdf` là bản mô tả kiến trúc gốc mà việc tách đôi này bám theo.

Tài liệu API là thứ duy nhất ở đây được đọc từ ngoài repo. Số hiệu trường
trong đó được chép tay từ `shared/proto/*.cpp`, nên **mọi thay đổi dưới
`shared/proto/` phải được sửa kèm trong cùng một commit** — đúng quy tắc đã
ràng `shared/proto/` với hai tệp `.proto` bên `s2t-dgpu`.

## Bố cục cây nguồn

```
s2t_qt.pro              TEMPLATE = subdirs, build cả hai
shared/                 dùng chung, nạp bằng include() chứ không phải thư viện tĩnh
  shared.pri            proto3 + HPACK + HTTP/2 client + nhật ký
  server.pri            shared.pri + HTTP/2 server + gRPC server
  proto/                bộ mã proto3 và bản mirror C++ của ba service
  grpc/                 HPACK, HTTP/2 (cả hai chiều), gRPC (cả hai chiều)
  core/Logger.*         nhật ký, chung cho cả hai
s2t-qt-server/          Server buffer
s2t-qt-client/          giao diện điều hành viên
tools/                  build/deploy, mock adapter, valgrind, unit systemd
```

`shared/` được nạp bằng `include(...pri)` vào từng `.pro` chứ không build
thành thư viện tĩnh: hai kit (MinGW trên Windows, gcc trên RHEL 9) khi đó
không phải thống nhất thứ tự link, và không có bước build nào phải nhớ. Đổi
lại, phần dùng chung được biên dịch hai lần — với ~5000 dòng thì đó là cái giá
không đáng bàn.

## Vì sao tầng giao thức vẫn viết tay

Kit Qt trên máy đích **không có Qt::Grpc, không có Qt::Protobuf, không có
protoc, không có vcpkg**. Thay vì thêm một chuỗi phụ thuộc build vào máy đã
triển khai, `shared/proto/` và `shared/grpc/` tự cài đặt phần cần dùng:

| Thư mục | Nội dung |
|---|---|
| `shared/proto/` | bộ mã proto3 hai chiều, cùng bản mirror C++ của `asr_session.proto`, `speaker_registry.proto` và `buffer_admin.proto` |
| `shared/grpc/` | HPACK (RFC 7541) có Huffman và bảng động; HTTP/2 **client** và **server**, cả hai đều chặn và mỗi kết nối một luồng; gRPC unary hai chiều |

Tổng phụ thuộc ngoài: **không có gì ngoài Qt.**

Việc tách đôi làm câu trả lời đó mạnh thêm chứ không yếu đi: nửa server không
cần Qt Multimedia lẫn Qt Widgets, nên một máy chủ không giao diện chỉ cần
`qt6-qtbase-devel`.

## Luồng và kênh

Nguyên tắc không đổi ở cả hai bên: **một mối lo một luồng, một kênh gRPC một
luồng**, để việc tuần tự hoá một khối trạng thái nhiều megabyte không bao giờ
xếp trước một gói audio 160 ms.

`s2t-qt-client`

| Luồng | Sở hữu | Việc |
|---|---|---|
| GUI | `SessionController`, mọi widget | vẽ, điều phối |
| `audio-capture` | `QAudioSource` | đệm 20 ms → `AudioQueue` có chặn |
| session worker | một kênh | `start_session` → vòng `push_audio` → `stop_session` |
| state poller | một kênh | `get_live_state` mỗi 200 ms |
| `rpc-lane-0..2` | mỗi luồng một kênh | soát, audio, sửa, đăng ký, trace |

`s2t-qt-server`

| Luồng | Sở hữu | Việc |
|---|---|---|
| main | `QTcpServer`, `BufferHub` | nhận kết nối, dọn phiên đã hết hạn |
| `http2-conn-N` | một socket client | đọc khung, gọi handler, ghi trả lời |
| `forward-<phiên>` | một `BackendSession` | rút hàng đợi, đẩy lên tầng suy luận, rồi xả nốt khi dừng |
| `triton-admin` / `riva-admin` | một kênh | `ping` và danh sách mô hình |
| `upstream-probe` | không kênh riêng | gọi qua kênh quản trị của backend mỗi 5 giây |

`state-<phiên>` và `relay-lane-N` **đã biến mất**. Bản chép nằm ngay trong
`SessionBuffer` nên không còn gì để làm mới, và không còn RPC nào được chuyển
tiếp nên không còn nhóm kênh nào để chuyển tiếp qua.

Số luồng của server tăng theo *số điều hành viên và số cuộc họp*, không theo
số request — mọi RPC ở đây đều là unary.

## Những hành vi được giữ lại có chủ ý

Mỗi mục dưới đây tồn tại vì một sự cố cụ thể, và lý do nằm ngay tại chỗ trong
mã nguồn. Việc tách đôi **di chuyển** vài mục sang server chứ không bỏ mục nào.

- **Mở thiết bị trước khi tạo phiên.** Lỗi driver đọc thành "không ghi được",
  thay vì để lại một cuộc họp rỗng trên server mà chẳng bao giờ có gì tới.
- **`seq` bất biến + chỉ thử lại lỗi vận chuyển.** Chỉ `UNAVAILABLE`,
  `DEADLINE_EXCEEDED` và `CANCELLED` được thử lại. `INTERNAL` thì không: đó
  chính là mã adapter trả về khi server *có thể* đã tiêu thụ audio, nên thử
  lại mù sẽ nhân đôi từ. **Quy tắc này giờ chạy ở cả hai chặng** — client với
  buffer, và buffer với tầng suy luận.
- **Hai hàng đợi báo riêng.** `ACK` nghĩa là *đã nằm chắc trong bộ đệm*, không
  bao giờ là *suy ra*. "Hàng đợi máy này" và "Hàng đợi server AI" là hai con
  số khác nhau và được hiện như vậy. Sau khi tách, con số thứ hai đọc thẳng từ
  `sourceSeenSec` của tầng suy luận, nên nó tính cả phần đang nằm trong bộ đệm.
- **Rào chắn drain trước `stop_session`.** `stop_session` chỉ được gọi lên
  tầng suy luận sau khi gói cuối cùng đã đi; điều đó được bảo đảm bằng cách
  giữ cả hai trên **cùng một luồng** của `SessionBuffer`.
- **Tạm dừng thì bỏ audio ngay lúc thu**, để lời nói lúc đang tạm dừng không
  bao giờ được gửi muộn như thể nó là trực tiếp.
- **Nhận dạng thiết bị được kiểm tra lại theo chu kỳ.** Hệ điều hành có thể
  tái sử dụng một endpoint audio sau khi rút, và một endpoint cũ vẫn có thể
  giữ luồng "đang chạy" trong khi chỉ đưa ra im lặng.
- **`expected_speakers` ba trạng thái.** Không có khoá = khớp toàn bộ registry;
  danh sách rỗng tường minh = không gán tên đã đăng ký nào.
- **Ranh giới commit được tôn trọng ở phía client.** Từ nằm sau ranh giới hiện
  ở dạng chỉ đọc.
- **Đồng thuận lạc quan cho mọi lần sửa** qua `base_revision`.
- **Tên người thao tác không bao giờ được nhớ giữa các lần chạy.**
- **Ghép đoạn thì từ chối chứ không cắt bớt.** Quá 200 đoạn hoặc 300 giây là
  từ chối thẳng.
- **Bộ đệm có giới hạn thì dừng ồn ào.** Đầy bộ đệm phiên là
  `RESOURCE_EXHAUSTED`, không phải im lặng bỏ gói: một lỗ trong audio là thứ
  không tầng nào phía sau phát hiện được.

## Build

Cùng một cây nguồn cho hai kit. Phần duy nhất phụ thuộc nền tảng nằm ở
`s2t-qt-client`: lớp đệm console trong `main.cpp`, phép kiểm tra màn hình bên
cạnh nó, và bộ lọc tệp cho công cụ điều khiển reSpeaker.

**RHEL 9** (đích đã kiểm: 9.8, glibc 2.34, gcc 11.5, x86_64):

```bash
# chỉ server (máy không giao diện):
sudo dnf install gcc-c++ make qt6-qtbase-devel
tools/build_rhel9.sh server

# thêm client:
sudo dnf install qt6-qtmultimedia-devel qt6-qtbase-gui
tools/build_rhel9.sh                      # cả hai, vào ../build-rhel
```

Script tồn tại vì một lý do: RHEL 9 để Qt 5 và Qt 6 cạnh nhau và `qmake` trần
ở đó là của Qt 5. Nó tìm một `qmake` của Qt 6 (`qmake6`, `qmake-qt6`,
`/usr/lib64/qt6/bin/qmake`, hoặc `$QMAKE6`) và kiểm tra phiên bản trước khi
dùng. Làm tay thì là:

```bash
mkdir ../build-rhel && cd ../build-rhel
qmake6 ../s2t_qt/s2t_qt.pro && make -j"$(nproc)"
```

`-std=c++17` là đủ, nên gcc 11.5 thoải mái.

**Windows** (kit máy trạm đã triển khai):

```
qmake s2t_qt.pro
mingw32-make -j8
```

Qt 6.11.2 MinGW 64-bit, GCC 13.1. Qt 6.2 là sàn trên cả hai nền tảng — mỗi
`.pro` kiểm tra phiên bản trước và dừng với lý do có tên, thay vì để lộ ra một
trang lỗi thiếu header. `-Wall -Wextra` được đặt trong `.pro` chứ không phó
mặc cho kit, và cây nguồn build sạch dưới nó trên cả hai kit.

## Chạy Server buffer

```bash
# Triton — cổng gRPC 8011, KHÔNG phải cổng HTTP 8010
s2t-qt-server --listen 0.0.0.0:8800 --token <token-client> \
              --backend triton --upstream 192.168.1.47:8011 \
              --model asr_diar_session

# Riva
s2t-qt-server --listen 0.0.0.0:8800 --token <token-client> \
              --backend riva --upstream 192.168.1.47:50051 --language vi-VN

s2t-qt-server --help          # mọi tuỳ chọn
s2t-qt-server --show-config   # cấu hình đã nạp, không khởi động
```

Hoặc bằng tệp cấu hình, cách nên dùng khi triển khai:

```bash
sudo install -m 600 tools/s2t-qt-server.conf.sample /etc/s2t-qt-server.conf
sudo install -m 644 tools/s2t-qt-server.service /etc/systemd/system/
sudo systemctl enable --now s2t-qt-server
journalctl -u s2t-qt-server -f
```

Tệp cấu hình chứa hai token dạng chữ thường, nên để quyền `600`.
`.gitignore` đã chặn tên `s2t-qt-server.conf` để một bản thật không lọt vào
repo.

## Phiên sống sót qua lần khởi động lại

Mặc định hàng đợi chỉ nằm trong RAM. Đặt `buffer/journal_dir` (hoặc
`--journal-dir`) là bật tính năng này:

```ini
[buffer]
journal_dir=/var/lib/s2t-qt-server
durability=os          # os | fsync
journal_keep=queue     # queue | session
segment_bytes=16777216
orphan_timeout_sec=1800
```

Mỗi phiên có một nhật ký nối-thêm, chia thành phân đoạn. Hai quy tắc về **thứ
tự** làm nên toàn bộ tính đúng đắn:

1. Bản ghi gói được ghi **trước** khi client được ACK. ACK nghĩa là "đã nằm
   chắc trong bộ đệm", và điều đó phải còn đúng sau khi khởi động lại — nếu
   không thì nó chưa bao giờ đúng. Ghi hỏng là **từ chối gói**, không phải ghi
   một dòng cảnh báo.
2. Bản ghi tiến độ được ghi **sau** khi đẩy lên tầng suy luận thành công. Chết
   giữa hai việc đó thì `seq` ấy được gửi lại ở lần khởi động sau, và tính bất
   biến theo `seq` của tầng suy luận biến bản trùng thành vô hại. Ghi ngược lại
   thì mất hẳn gói.

Với client thì **một lần khởi động lại là vô hình**: nó nhận vài lỗi vận
chuyển, vòng thử lại sẵn có gửi lại đúng `seq` cũ, và cuộc họp đi tiếp. Hàng
đợi 60 giây mặc định của client hấp thụ trọn một lần khởi động lại thông thường.

| Chọn | Chịu được | Giá |
|---|---|---|
| `durability=os` (mặc định) | tiến trình chết, máy chủ khởi động lại | không đáng kể |
| `durability=fsync` | thêm cả mất điện đột ngột | một `fsync` mỗi gói |
| `journal_keep=queue` (mặc định) | — | đĩa bám theo độ sâu hàng đợi |
| `journal_keep=session` | — | giữ cả cuộc họp làm bản lưu (~96 kB/giây) |

**Những gì nhật ký không làm.** Nó không làm bộ đệm chứa được nhiều hơn — đầy
vẫn là `RESOURCE_EXHAUSTED`. Nó không cứu được một phiên mà **tầng suy luận**
đã bỏ: nếu adapter quên phiên trong lúc máy chủ đệm nằm xuống, gói đầu tiên
được gửi lại sẽ nhận một lỗi không phải lỗi vận chuyển và phiên kết thúc ồn ào,
đúng như mọi lỗi chí mạng khác.

Một phiên được khôi phục mà không client nào quay lại nhận sẽ bị đóng sau
`orphan_timeout_sec` (mặc định 30 phút) — phần chưa gửi vẫn được đẩy lên trước
khi đóng. Không có mốc này thì một máy chủ khởi động lại vài lần sẽ tích lại
những cuộc họp không bao giờ kết thúc.

**Một thư mục nhật ký chỉ một máy chủ.** Hai tiến trình dùng chung sẽ cùng khôi
phục một phiên và cùng gửi lại một gói, tức nhân đôi chữ trong bản chép — hỏng
theo kiểu tệ nhất mà dự án này biết. Máy chủ giữ một `QLockFile` trong thư mục
đó và **từ chối khởi động** nếu đã có ai giữ, kèm pid và tên máy. Khoá ghi pid
chứ không dựa vào thời gian, nên một cú `SIGKILL` không để lại khoá chết chặn
mất đúng cái lần khởi động lại mà nhật ký sinh ra để phục vụ.

## Self-test

```bash
s2t-qt-server --selftest         # bộ mã hai chiều + đóng khung + loopback + cả chuỗi
s2t-qt-server --selftest-codec   # chỉ bộ mã, không mở socket
s2t-qt-server --probe 192.168.1.47:8011 --token <token>

# Cả luồng, với grpcio thật ở đầu client (stub: xem đầu tệp)
python3 tools/demo_flow.py --target 127.0.0.1:18800 --token <token> \
                          --wav mau.wav --realtime

s2t-qt-client --selftest                       # proto3 + HPACK
s2t-qt-client --selftest-net 127.0.0.1:18700   # với tools/mock_adapter.js
s2t-qt-client --probe 192.168.1.47:8800 --token <token>
```

`s2t-qt-server --selftest` là bài kiểm tra đáng giá nhất ở đây, vì nó chạy
đúng phần mã không có thư viện nào đứng sau:

1. **Bộ mã** — proto3 vòng tròn theo *cả hai chiều* (client ghi/server đọc và
   ngược lại), đóng khung gRPC, và mã hoá phần trăm cho `grpc-message` tiếng
   Việt.
2. **Loopback** — dựng một `grpc::Server` thật trên cổng loopback rồi lấy
   chính `Http2Client` đã có gọi vào: round trip, deadline, một trả lời vài
   megabyte (ép chạy điều khiển luồng HTTP/2 thật), phương thức không tồn tại,
   token sai, token thiếu.
3. **Cả chuỗi** — một tầng suy luận giả, `BufferHub` và `BufferService` thật
   đứng trước nó, và một client thật lái cả hai. Kiểm những thứ chỉ riêng bộ
   đệm mới có thể làm sai: thứ tự gói, rào chắn drain trước `stop_session`,
   phát lại ACK cho `seq` gửi lại, và bộ đệm trạng thái phục vụ nhiều người
   đọc bằng một lần hỏi.
4. **Khởi động lại** — hạ một máy chủ đệm giữa cuộc họp với gói còn đang chờ,
   dựng một cái mới trên cùng thư mục nhật ký, rồi kiểm: phần tồn đọng đi lên
   đúng thứ tự, không trùng, không thiếu; `seq` gửi lại vẫn phát lại ACK cũ; và
   client ghi tiếp từ `seq` kế. Kèm một nhật ký kết thúc bằng bản ghi ghi dở —
   hình dạng bình thường sau một cú chết — phải đọc ra được mọi thứ trước chỗ
   rách.

`s2t-qt-client --selftest-net` chạy với `tools/mock_adapter.js`, một máy chủ
HTTP/2 bằng Node không cần phụ thuộc. http2 của Node là nghttp2, nên nó mã hoá
Huffman và dùng bảng động HPACK của riêng nó — đó chính là điểm cần: nó chứng
minh client làm việc được với một cài đặt độc lập, chứ không chỉ với chính nó.

## Kiểm tra tương thích với gRPC thật

`tools/interop_check.py` làm điều mà không self-test nào làm được: cho **grpc
C-core thật** (grpcio 1.80) gọi vào Server buffer tự viết. Client và server ở
đây dùng chung một bản HPACK và một bộ mã proto3, nên chúng có thể đồng ý với
nhau mà cả hai cùng sai; grpc C-core thì không đồng ý với ai cả.

Nó cũng kiểm luôn rằng tệp `.proto` in trong `docs/danh-sach-api.md` là thật —
các stub dùng ở đó được sinh ra từ chính khối văn bản đó bằng `protoc`. Xem
phần chú thích đầu tệp để biết cách chạy. Máy RHEL có sẵn `python3 -m
grpc_tools.protoc`; máy Windows thì không có protoc lẫn Python.

Chạy lần gần nhất (2026-08-25, trên RHEL): 9/9 khẳng định đạt, bao gồm việc
`grpc-message` tiếng Việt giải mã đúng và một mã trạng thái từ tầng suy luận đi
xuyên qua bộ đệm về tới người gọi.

`tools/restart_check.py` là bài thứ hai cùng loại, cho phần sống-sót-qua-khởi-
động-lại: nó **giết máy chủ bằng `SIGKILL`** giữa cuộc họp, với grpc thật ở cả
hai đầu. `--selftest` trong C++ kiểm toàn bộ logic khôi phục nhưng chỉ *tháo
đối tượng*, nên nó không chứng minh được điều quan trọng nhất — rằng bản ghi đã
nằm ngoài tiến trình trước khi client được ACK. `SIGKILL` không chạy destructor
và không flush gì cả.

Chạy lần gần nhất (2026-08-25, trên RHEL): đạt ở cả `durability=os` lẫn
`durability=fsync`, và ở cả hai chế độ `journal_keep`.

## Kiểm tra bộ nhớ và gỡ lỗi trên RHEL

Máy RHEL có gdb 16.3 và valgrind 3.26, và cả hai đều không được việc với bản
build `-O2` không có frame pointer, nên build có sẵn một chế độ cho chúng:

```bash
tools/build_rhel9.sh memcheck        # -O1 -g3 -fno-omit-frame-pointer
cd ../build-rhel && ../s2t-qt/tools/run_valgrind.sh
```

`run_valgrind.sh` mặc định chạy `s2t-qt-server --selftest` có chủ ý: chế độ đó
lái đúng phần mã tự viết — bộ mã proto3 hai chiều, HPACK với bảng động, đóng
khung HTTP/2 ở cả hai đầu một socket thật, và cả một phiên đi qua bộ đệm — mà
không cần GUI, không cần thiết bị.

`tools/valgrind.supp` chỉ chặn tiếng ồn của bên thứ ba. Không có gì dưới
`shared/`, `s2t-qt-server/` hay `s2t-qt-client/` bị chặn, và đó là lý do danh
sách đó được giữ ngắn.

Dưới gdb, mọi luồng đều có tên (`http2-conn-N`, `forward-<phiên>`,
`relay-lane-N`, `upstream-probe`, `audio-capture`, `state-poller`), nên
`thread apply all bt` đọc ra như hai bảng luồng ở trên chứ không phải một danh
sách số.

## Cấu hình client

Sửa trong **Cấu hình** và lưu bằng `QSettings` vào chỗ của từng nền tảng —
registry `HKCU\Software\s2t\s2t_qt` trên Windows, `~/.config/s2t/s2t_qt.conf`
trên RHEL. Cùng bộ khoá: `host:port` **của Server buffer**, Bearer token,
microphone, chuỗi con tên thiết bị phải khớp, tần số/kênh, độ dài hàng đợi có
chặn, đường dẫn `xvf_host`, pipeline trace, và nhịp phát lại tệp.

Token lưu dạng chữ thường trong tệp đó, nên trên Linux tệp nên là `chmod 600`
— `QSettings` tạo nó ở `0600` sẵn, nhưng một thư mục config chép giữa hai máy
sẽ không giữ quyền đó.

Client **không còn** biết địa chỉ hay token của tầng suy luận: đó là việc của
Server buffer và không bao giờ rời khỏi nó.

## Từ có độ tin cậy thấp

Giao diện trình duyệt cũ bỏ hẳn mọi token dưới **0.75** khỏi dòng thời gian,
khiến chính kiểu hiển thị "độ tin cậy thấp" của nó không bao giờ dùng tới. Mặc
định đó được giữ nguyên, nhưng nút **HIỆN TỪ YẾU** giờ cho điều hành viên thấy
tất cả, vẽ theo kiểu độ-tin-cậy-thấp, mà không cần build lại.

## Những chỗ còn thiếu

- **Phiên chỉ sống qua lần khởi động lại khi `buffer/journal_dir` được đặt.**
  Để trống thì hàng đợi chỉ nằm trong RAM và một lần khởi động lại mất mọi
  cuộc họp đang mở — `push_audio` cho phiên cũ trả `NOT_FOUND`, và thông báo
  lỗi nói rõ là do nhật ký đang tắt.
- **Nhật ký không làm bộ đệm to ra.** Đầy bộ đệm vẫn là `RESOURCE_EXHAUSTED`;
  nhật ký làm cho phần đang chờ *sống sót*, chứ không làm nó *chứa được nhiều
  hơn*.
- **Giải mã `.m4a` cần `ffmpeg` trên `PATH`.** Không có thì ứng dụng nói thẳng
  ra thay vì báo tệp hỏng; WAV PCM 16-bit thì không cần gì.
- **Chế độ ticker là dải chữ chảy có màu**, không phải các khối teleprompter
  ngang như của trình duyệt.
- **Nhãn *kiến trúc* mô hình trong bảng nghiệm thu được biên dịch cứng.** Bảng
  *trạng thái* mô hình bên cạnh mới là bằng chứng đọc trực tiếp từ Triton.
- **Chỉ Qt 6.** Thu là `QAudioSource`/`QMediaDevices`, phát là
  `QMediaPlayer::setAudioOutput`; không có cách viết Qt 5 nào để lùi về.
- **Client cần màn hình.** Qua một phiên `ssh` trần, nó thoát kèm giải thích và
  danh sách chế độ không cần màn hình. `s2t-qt-server` thì ngược lại: nó không
  bao giờ cần màn hình.
- **Công cụ điều khiển reSpeaker là tuỳ chọn trên Linux.** Để trống thì nút
  denoise và bộ ghi bằng chứng A/B báo là chưa cấu hình công cụ; mọi thứ khác
  vẫn chạy.
