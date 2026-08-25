# Phần dùng chung của s2t-qt-server và s2t-qt-client.
#
# Cả hai bên đều cần đúng ba thứ: bộ mã proto3 viết tay, tầng HTTP/2 + HPACK,
# và nhật ký.  Không có thư viện ngoài nào ở đây - máy đích không có protoc,
# Qt::Grpc hay vcpkg, nên toàn bộ giao thức nằm trong shared/proto và
# shared/grpc.
#
# Được nạp bằng include($$PWD/../shared/shared.pri) từ mỗi .pro, chứ không
# build thành thư viện tĩnh: hai kit (MinGW trên Windows, gcc trên RHEL 9)
# khi đó không phải thống nhất thứ tự link, và một .pri không thêm bước build
# nào cần nhớ.

QT += network

INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/proto/ProtoWire.cpp \
    $$PWD/proto/AsrSession.cpp \
    $$PWD/proto/SpeakerRegistry.cpp \
    $$PWD/proto/BufferAdmin.cpp \
    $$PWD/proto/RivaAsr.cpp \
    $$PWD/proto/TritonInfer.cpp \
    $$PWD/grpc/Hpack.cpp \
    $$PWD/grpc/Http2Client.cpp \
    $$PWD/grpc/GrpcChannel.cpp \
    $$PWD/grpc/AsrClient.cpp \
    $$PWD/core/Logger.cpp

HEADERS += \
    $$PWD/proto/ProtoWire.h \
    $$PWD/proto/AsrSession.h \
    $$PWD/proto/SpeakerRegistry.h \
    $$PWD/proto/BufferAdmin.h \
    $$PWD/proto/RivaAsr.h \
    $$PWD/proto/TritonInfer.h \
    $$PWD/grpc/Hpack.h \
    $$PWD/grpc/Http2Client.h \
    $$PWD/grpc/GrpcChannel.h \
    $$PWD/grpc/AsrClient.h \
    $$PWD/grpc/Methods.h \
    $$PWD/core/Logger.h
