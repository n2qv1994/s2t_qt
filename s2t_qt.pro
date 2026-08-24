QT += widgets network multimedia

CONFIG += c++17
CONFIG -= app_bundle

TARGET = s2t_qt

# Qt 6 only, and deliberately checked here.  Capture is QAudioSource +
# QMediaDevices and playback is QMediaPlayer::setAudioOutput; none of those
# exist in Qt 5 in a form a #if could paper over.  RHEL 9 ships both Qt 5 and
# Qt 6, and `qmake` there is the Qt 5 one - so without this check the first
# sign of using the wrong one is a screenful of missing-header errors.
lessThan(QT_MAJOR_VERSION, 6) {
    error("s2t_qt cần Qt 6 (đang dùng Qt $$QT_VERSION). Trên RHEL 9 hãy gọi qmake6, không phải qmake.")
}
!versionAtLeast(QT_VERSION, 6.2.0) {
    error("s2t_qt cần Qt 6.2 trở lên (đang dùng Qt $$QT_VERSION).")
}
!qtHaveModule(multimedia) {
    error("Thiếu module Qt Multimedia. RHEL 9: sudo dnf install qt6-qtmultimedia-devel")
}

# Vietnamese strings are compiled in as UTF-8 literals; MinGW needs to be told
# so explicitly or it reads the sources in the system codepage.  GCC on RHEL
# already defaults to UTF-8 both ways, but stating it keeps the two toolchains
# emitting identical bytes no matter what locale the build machine is in.
win32-g++: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
linux-g++*: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

# The README claims this builds clean under -Wall -Wextra.  Put it in the
# build rather than in the prose, so the claim is checked on every kit.
QMAKE_CXXFLAGS_WARN_ON *= -Wall
QMAKE_CXXFLAGS_WARN_ON *= -Wextra

# gdb 16.3 and valgrind 3.26 are both installed on the RHEL host and neither
# is worth much against an -O2 build with no frame pointers.  `qmake6
# CONFIG+=memcheck` keeps optimisation low, frame pointers in place and full
# debug info present, which is what memcheck needs to name the frame that
# leaked.  See tools/run_valgrind.sh.
memcheck {
    CONFIG += force_debug_info
    QMAKE_CXXFLAGS += -O1 -g3 -fno-omit-frame-pointer
    QMAKE_LFLAGS += -rdynamic
}

# Which way the log flag points when nothing overrides it at run time.
# `qmake6 CONFIG+=develop` builds a binary that writes its log to a file by
# default; a plain build logs to the console.  Either way --log-mode,
# S2T_LOG_MODE and the setting in "Cấu hình" still have the last word, so this
# only picks the starting position - see core/Logger.h.
develop {
    DEFINES += S2T_LOG_DEFAULT_DEVELOP
}

unix:!macx {
    target.path = /usr/local/bin
    INSTALLS += target
}

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    proto/ProtoWire.cpp \
    proto/AsrSession.cpp \
    proto/SpeakerRegistry.cpp \
    grpc/Hpack.cpp \
    grpc/Http2Client.cpp \
    grpc/GrpcChannel.cpp \
    grpc/AsrClient.cpp \
    audio/WavIo.cpp \
    audio/Transcode.cpp \
    audio/AudioCapture.cpp \
    audio/MicDenoise.cpp \
    core/AppConfig.cpp \
    core/Logger.cpp \
    core/AudioQueue.cpp \
    core/RpcExecutor.cpp \
    core/SessionTypes.cpp \
    core/SessionWorker.cpp \
    core/StatePoller.cpp \
    core/TranscriptModel.cpp \
    core/SessionController.cpp \
    core/SelfTest.cpp \
    ui/TimelineView.cpp \
    ui/Dialogs.cpp \
    ui/ReviewPanel.cpp \
    ui/EnrollDialog.cpp \
    ui/TraceWindow.cpp \
    ui/EvidenceWindow.cpp \
    ui/DiagnosticsWindow.cpp \
    ui/LogControls.cpp

HEADERS += \
    mainwindow.h \
    proto/ProtoWire.h \
    proto/AsrSession.h \
    proto/SpeakerRegistry.h \
    grpc/Hpack.h \
    grpc/Http2Client.h \
    grpc/GrpcChannel.h \
    grpc/AsrClient.h \
    audio/WavIo.h \
    audio/Transcode.h \
    audio/AudioCapture.h \
    audio/MicDenoise.h \
    core/AppConfig.h \
    core/Logger.h \
    core/AudioQueue.h \
    core/RpcExecutor.h \
    core/SessionTypes.h \
    core/SessionWorker.h \
    core/StatePoller.h \
    core/TranscriptModel.h \
    core/SessionController.h \
    core/SelfTest.h \
    ui/TimelineView.h \
    ui/Dialogs.h \
    ui/ReviewPanel.h \
    ui/EnrollDialog.h \
    ui/TraceWindow.h \
    ui/EvidenceWindow.h \
    ui/DiagnosticsWindow.h \
    ui/LogControls.h
