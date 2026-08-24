# Hai chương trình, một cây nguồn.
#
#   s2t-qt-server  "Server buffer" trong sơ đồ luồng tổng thể: nhận audio từ
#                  các client, đệm lại, rồi đẩy lên tầng suy luận (adapter ->
#                  Triton).  Không có giao diện, chạy như một dịch vụ.
#   s2t-qt-client  Giao diện Qt Widgets cho điều hành viên.  Chỉ nói chuyện
#                  với Server buffer, không còn nối thẳng lên adapter.
#
# shared/ là phần dùng chung của hai bên: bộ mã proto3, HPACK, HTTP/2 và nhật
# ký.  Nó không phải một dự án thứ ba - hai .pro dưới đây nạp nó bằng
# include(), nên vẫn chỉ có đúng hai sản phẩm build ra.
TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = \
    s2t-qt-server \
    s2t-qt-client

s2t-qt-server.file = s2t-qt-server/s2t-qt-server.pro
s2t-qt-client.file = s2t-qt-client/s2t-qt-client.pro
