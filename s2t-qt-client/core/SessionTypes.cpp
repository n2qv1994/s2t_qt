#include "SessionTypes.h"

QString micStatusKey(MicStatus status)
{
    switch (status) {
    case MicStatus::Idle: return QStringLiteral("");
    case MicStatus::Starting: return QStringLiteral("starting");
    case MicStatus::Recording: return QStringLiteral("recording");
    case MicStatus::Paused: return QStringLiteral("paused");
    case MicStatus::NetworkReconnecting: return QStringLiteral("network_reconnecting");
    case MicStatus::DeviceReconnecting: return QStringLiteral("device_reconnecting");
    case MicStatus::Finalizing: return QStringLiteral("finalizing");
    case MicStatus::Stopped: return QStringLiteral("stopped");
    case MicStatus::Error: return QStringLiteral("error");
    }
    return QString();
}

QString micStatusLabel(MicStatus status)
{
    switch (status) {
    case MicStatus::Idle: return QStringLiteral("SẴN SÀNG");
    case MicStatus::Starting: return QStringLiteral("Đang khởi động...");
    case MicStatus::Recording: return QStringLiteral("SẴN SÀNG · đang ghi");
    case MicStatus::Paused: return QStringLiteral("SẴN SÀNG · tạm dừng");
    case MicStatus::NetworkReconnecting: return QStringLiteral("Đang kết nối lại mạng...");
    case MicStatus::DeviceReconnecting: return QStringLiteral("Mic mất kết nối - đang chờ cắm lại");
    case MicStatus::Finalizing: return QStringLiteral("Đang kết thúc...");
    case MicStatus::Stopped: return QStringLiteral("Đã dừng");
    case MicStatus::Error: return QStringLiteral("KHÔNG SẴN SÀNG");
    }
    return QString();
}
