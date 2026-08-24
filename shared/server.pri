# Nửa "phía nhận" của tầng giao thức: chỉ s2t-qt-server dùng.
#
# Tách khỏi shared.pri có chủ đích.  Client không bao giờ mở cổng lắng nghe,
# nên nó không cần - và không nên chứa - một máy chủ HTTP/2 chưa từng được
# gọi tới.  Cùng bộ HPACK và cùng cách đóng khung với shared/grpc/Http2Client,
# nhưng đi theo chiều ngược lại.

include($$PWD/shared.pri)

SOURCES += \
    $$PWD/grpc/Http2Server.cpp \
    $$PWD/grpc/GrpcServer.cpp

HEADERS += \
    $$PWD/grpc/Http2Server.h \
    $$PWD/grpc/GrpcServer.h
