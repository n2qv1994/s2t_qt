QT += core network sql
QT -= gui

CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = s2t-qt-server

# Qt 6 only, and checked here rather than left to the first missing header.
# RHEL 9 ships Qt 5 and Qt 6 side by side and plain `qmake` there is the Qt 5
# one; unlike the client this half would *almost* build under it, which is
# worse - it would fail somewhere in the middle instead of at the top.
lessThan(QT_MAJOR_VERSION, 6) {
    error("s2t-qt-server cần Qt 6 (đang dùng Qt $$QT_VERSION). Trên RHEL 9 hãy gọi qmake6, không phải qmake.")
}
!versionAtLeast(QT_VERSION, 6.2.0) {
    error("s2t-qt-server cần Qt 6.2 trở lên (đang dùng Qt $$QT_VERSION).")
}

# Bộ mã proto3, HPACK, HTTP/2 (cả hai chiều) và nhật ký, dùng chung với client.
include($$PWD/../shared/server.pri)

INCLUDEPATH += $$PWD

# No Qt Multimedia, no widgets: this half never opens a device and never draws
# anything.  Audio reaches it as bytes on a socket and leaves the same way.

win32-g++: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
linux-g++*: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

QMAKE_CXXFLAGS_WARN_ON *= -Wall
QMAKE_CXXFLAGS_WARN_ON *= -Wextra

memcheck {
    CONFIG += force_debug_info
    QMAKE_CXXFLAGS += -O1 -g3 -fno-omit-frame-pointer
    QMAKE_LFLAGS += -rdynamic
}

develop {
    DEFINES += S2T_LOG_DEFAULT_DEVELOP
}

unix:!macx {
    target.path = /usr/local/bin
    INSTALLS += target
}

SOURCES += \
    main.cpp \
    ServerConfig.cpp \
    RpcLane.cpp \
    backend/InferenceBackend.cpp \
    backend/TritonBackend.cpp \
    backend/RivaBackend.cpp \
    CampPlusClient.cpp \
    LiveTranscript.cpp \
    SessionJournal.cpp \
    SessionStore.cpp \
    SessionBuffer.cpp \
    BufferHub.cpp \
    BufferService.cpp \
    ServerSelfTest.cpp

HEADERS += \
    ServerConfig.h \
    RpcLane.h \
    backend/InferenceBackend.h \
    backend/TritonBackend.h \
    backend/RivaBackend.h \
    CampPlusClient.h \
    LiveTranscript.h \
    SessionJournal.h \
    SessionStore.h \
    SessionBuffer.h \
    BufferHub.h \
    BufferService.h \
    ServerSelfTest.h
