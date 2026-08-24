# s2t_qt — Server buffer + client Qt/C++ cho hệ thống S2T

Hai chương trình Qt 6, một cây nguồn:

| Dự án | Là gì | Cần gì |
|---|---|---|
| **`s2t-qt-server`** | *Server buffer* trong sơ đồ luồng tổng thể (`docs/slide.pdf`). Nhận audio từ các client qua gRPC, đệm lại, rồi đẩy lên tầng suy luận. Không có giao diện. | Qt Core + Network |
| **`s2t-qt-client`** | Giao diện điều hành viên: thu microphone, xem transcript trực tiếp, soát/sửa, đăng ký giọng. Chỉ nói chuyện với Server buffer. | Qt Widgets + Multimedia + Network |

```
Windows/RHEL client        Server buffer            tầng suy luận
┌────────────────┐        ┌──────────────┐        ┌──────────────────┐
│ s2t-qt-client  │ gRPC   │ s2t-qt-server│ gRPC   │ grpc_session_    │  Triton
│ mic, UI, sửa   │──────► │ hàng đợi     │──────► │ adapter.py :8700 │─────────►
│                │ :8800  │ + chuyển tiếp│        │                  │ Denoise→VAD
└────────────────┘        └──────────────┘        └──────────────────┘ →ASR→Diar
                                                                        →Verify
```

Trước đây `s2t_qt` là một chương trình duy nhất nối thẳng lên adapter. Việc
tách đôi đặt hàng đợi audio ra khỏi máy trạm, và đó là toàn bộ lý do:

- **Sự cố mạng ở máy trạm không còn làm mất tiếng.** Client được trả lời ngay
  khi gói audio nằm chắc chắn trong bộ đệm của server; từ đó trở đi việc thử
  lại là chuyện của server, ở một chặng gần tầng suy luận hơn.
- **Tầng suy luận sập không làm dừng cuộc họp.** Audio vẫn được nhận và xếp
  hàng (mặc định 300 giây mỗi phiên). Đèn báo trên client chuyển vàng —
  *đang đệm* — chứ không đỏ.
- **Nhiều người xem một cuộc họp chỉ tốn một lần hỏi.** `get_live_state` được
  đệm trong 200 ms, nên mười client cùng theo dõi vẫn chỉ là một lần đọc
  trạng thái trên tầng suy luận.
- **Máy trạm chỉ cần mở đúng một cổng ra ngoài**, tới Server buffer. Địa chỉ
  và token của tầng suy luận không còn nằm trên máy của điều hành viên.

Phía tầng suy luận không đổi một dòng nào: Triton, adapter, sidecar CAM++ và
hai tệp `.proto` vẫn nguyên như cũ.

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
| `forward-<phiên>` | một kênh lên tầng suy luận | rút hàng đợi, `push_audio`, rồi `stop_session` |
| `state-<phiên>` | một kênh riêng | làm mới bộ đệm `get_live_state` |
| `relay-lane-0..N` | mỗi luồng một kênh | mọi RPC chuyển tiếp |
| `upstream-probe` | một kênh | kiểm tra tầng suy luận còn sống không |

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
s2t-qt-server --listen 0.0.0.0:8800 --token <token-client> \
              --upstream 192.168.1.47:8700 --upstream-token <token-adapter>
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

## Self-test

```bash
s2t-qt-server --selftest         # bộ mã hai chiều + đóng khung + loopback + cả chuỗi
s2t-qt-server --selftest-codec   # chỉ bộ mã, không mở socket
s2t-qt-server --probe 192.168.1.47:8700 --token <token>

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

- **Phiên không sống qua lần khởi động lại của server.** Bộ đệm nằm trong RAM
  và bản đồ phiên cũng vậy, nên sau khi khởi động lại, `push_audio` cho một
  phiên cũ trả về `NOT_FOUND` với thông báo nói rõ điều đó. Tầng suy luận vẫn
  còn phiên ấy; chỉ bộ đệm là không.
- **`buffer/spool_dir` là bản sao để đối chiếu, không phải nơi hàng đợi tràn
  vào.** Đầy bộ đệm vẫn là `RESOURCE_EXHAUSTED` dù có bật spool hay không.
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
