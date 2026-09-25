#include "InferenceBackend.h"

#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace {

// The client and the old Python adapter never agreed on one spelling for these
// keys, and both spellings are in deployed configs.  Accepting either costs one
// lookup and saves a meeting that would otherwise start at 16 kHz mono because
// the operator wrote "sample_rate" where we looked for "sampleRate".
QJsonValue pick(const QJsonObject &object, const char *snake, const char *camel)
{
    if (object.contains(QLatin1String(snake)))
        return object.value(QLatin1String(snake));
    if (object.contains(QLatin1String(camel)))
        return object.value(QLatin1String(camel));
    return QJsonValue();
}

bool boolOr(const QJsonValue &value, bool fallback)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble())
        return value.toDouble() != 0.0;
    // "true"/"1"/"yes" all appear in hand-written configs.
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1")
            || text == QLatin1String("yes"))
            return true;
        if (text == QLatin1String("false") || text == QLatin1String("0")
            || text == QLatin1String("no"))
            return false;
    }
    return fallback;
}

int intOr(const QJsonValue &value, int fallback)
{
    if (value.isDouble())
        return int(value.toDouble());
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

QString stringOr(const QJsonValue &value, const QString &fallback)
{
    if (value.isString() && !value.toString().trimmed().isEmpty())
        return value.toString().trimmed();
    return fallback;
}

} // namespace

BackendSessionConfig BackendSessionConfig::fromJson(const QString &json, QString *warning)
{
    BackendSessionConfig config;
    config.rawJson = json;
    if (warning)
        warning->clear();

    const QString trimmed = json.trimmed();
    if (trimmed.isEmpty())
        return config; // no config at all is legal: every default is usable

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // Deliberately not fatal.  Refusing to start a meeting because an
        // optional field was misspelled would turn a cosmetic mistake into a
        // lost recording; the operator gets told and the defaults apply.
        if (warning) {
            *warning = QStringLiteral("cấu hình phiên không phải JSON hợp lệ (%1) - đang dùng "
                                      "toàn bộ giá trị mặc định")
                           .arg(parseError.errorString());
        }
        return config;
    }

    const QJsonObject object = document.object();
    // `session_title` is the contract's spelling - see docs/danh-sach-api.md
    // section 7, and buildConfigJson() in s2t-qt-client/core/SessionWorker.cpp,
    // which is what the deployed client actually sends.  `title` is accepted
    // too because the selftest and hand-written configs use it.
    config.title = stringOr(pick(object, "session_title", "title"), config.title);
    config.sourceTotalSec = pick(object, "source_total_sec", "sourceTotalSec").toDouble(0.0);
    config.sampleRate =
        quint32(intOr(pick(object, "sample_rate", "sampleRate"), int(config.sampleRate)));
    config.channels = quint32(intOr(pick(object, "channels", "channels"), int(config.channels)));
    config.audioFormat = stringOr(pick(object, "audio_format", "audioFormat"), config.audioFormat);
    config.language = stringOr(pick(object, "language_code", "languageCode"), config.language);
    config.language = stringOr(pick(object, "language", "lang"), config.language);
    config.model = stringOr(pick(object, "model", "model_name"), config.model);
    config.diarization =
        boolOr(pick(object, "diarization", "enableSpeakerDiarization"), config.diarization);
    config.maxSpeakers = intOr(pick(object, "max_speakers", "maxSpeakerCount"), config.maxSpeakers);
    config.punctuation =
        boolOr(pick(object, "punctuation", "enableAutomaticPunctuation"), config.punctuation);
    config.wordTimeOffsets =
        boolOr(pick(object, "word_time_offsets", "enableWordTimeOffsets"), config.wordTimeOffsets);
    config.interimResults =
        boolOr(pick(object, "interim_results", "interimResults"), config.interimResults);
    config.vadChunkMs =
        quint32(intOr(pick(object, "vad_chunk_ms", "vadChunkMs"), int(config.vadChunkMs)));

    // The attendee list may arrive as a JSON array or as an already-encoded
    // string; Triton's expected_speakers_json input wants the string, so a list
    // is re-encoded rather than rejected.
    const QJsonValue expected = pick(object, "expected_speakers", "expectedSpeakers");
    if (expected.isArray()) {
        config.expectedSpeakersJson =
            QString::fromUtf8(QJsonDocument(expected.toArray()).toJson(QJsonDocument::Compact));
    } else if (expected.isString()) {
        config.expectedSpeakersJson = expected.toString();
    }

    const QJsonValue participants = pick(object, "participants", "participants");
    if (participants.isArray()) {
        for (const QJsonValue &value : participants.toArray())
            config.participants.append(value.toString());
    }
    config.securityLevel = stringOr(pick(object, "security_level", "securityLevel"), QString());
    config.mode = stringOr(pick(object, "mode", "mode"), config.mode);
    config.recordOnly = config.mode == QLatin1String("record_only");

    if (config.sampleRate == 0 || config.channels == 0) {
        // A zero here would divide by zero in the lag calculation and hand the
        // tier an unusable RecognitionConfig, so it is repaired loudly.
        if (warning) {
            *warning = QStringLiteral("cấu hình phiên khai báo sample_rate=%1, channels=%2 - "
                                      "đang dùng 16000/1 thay thế")
                           .arg(config.sampleRate)
                           .arg(config.channels);
        }
        if (config.sampleRate == 0)
            config.sampleRate = 16000;
        if (config.channels == 0)
            config.channels = 1;
    }
    return config;
}

namespace {

// Every key start_session understands.  The first eight are the contract the
// reference adapter enforces; the rest are this server's own documented
// extensions (docs/danh-sach-api.md §7) and are accepted so a hand-written
// config or a journal from an older build still starts.
const char *const kAllowedConfigKeys[] = {
    "conf_threshold", "source_total_sec", "expected_speakers", "pipeline_trace",
    "mode", "session_title", "participants", "security_level",
    "title", "sample_rate", "sampleRate", "channels", "audio_format", "audioFormat",
    "language", "language_code", "languageCode", "lang", "model", "model_name",
    "diarization", "enableSpeakerDiarization", "max_speakers", "maxSpeakerCount",
    "punctuation", "enableAutomaticPunctuation", "word_time_offsets", "enableWordTimeOffsets",
    "interim_results", "interimResults", "vad_chunk_ms", "vadChunkMs", "sourceTotalSec",
    "expectedSpeakers",
};

bool isStringList(const QJsonValue &value, int maxItems, int maxLength)
{
    if (!value.isArray())
        return false;
    const QJsonArray array = value.toArray();
    if (array.size() > maxItems)
        return false;
    for (const QJsonValue &item : array) {
        if (!item.isString() || item.toString().size() > maxLength)
            return false;
    }
    return true;
}

} // namespace

bool BackendSessionConfig::validateJson(const QString &json, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString trimmed = json.trimmed();
    if (trimmed.isEmpty())
        return true; // no config at all is legal

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("config_json phải là một đối tượng JSON (%1)")
                        .arg(parseError.errorString()));

    const QJsonObject object = document.object();
    QStringList unsupported;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        bool known = false;
        for (const char *key : kAllowedConfigKeys) {
            if (it.key() == QLatin1String(key)) {
                known = true;
                break;
            }
        }
        if (!known)
            unsupported << it.key();
    }
    if (!unsupported.isEmpty()) {
        unsupported.sort();
        return fail(QStringLiteral("config_json có trường không hỗ trợ: %1")
                        .arg(unsupported.join(QStringLiteral(", "))));
    }

    if (object.contains(QLatin1String("conf_threshold"))) {
        const QJsonValue value = object.value(QLatin1String("conf_threshold"));
        if (!value.isDouble() || value.toDouble() < 0.0 || value.toDouble() > 1.0)
            return fail(QStringLiteral("config.conf_threshold phải nằm trong [0, 1]"));
    }
    if (object.contains(QLatin1String("source_total_sec"))) {
        const QJsonValue value = object.value(QLatin1String("source_total_sec"));
        if (!value.isDouble() || value.toDouble() < 0.0)
            return fail(QStringLiteral("config.source_total_sec không được âm"));
    }
    if (object.contains(QLatin1String("pipeline_trace"))
        && !object.value(QLatin1String("pipeline_trace")).isBool()) {
        return fail(QStringLiteral("config.pipeline_trace phải là true/false"));
    }
    if (object.contains(QLatin1String("expected_speakers"))
        && !isStringList(object.value(QLatin1String("expected_speakers")), 256, 128)) {
        return fail(QStringLiteral("config.expected_speakers phải là danh sách chuỗi "
                                   "(tối đa 256 tên, mỗi tên 128 ký tự)"));
    }
    if (object.contains(QLatin1String("participants"))
        && !isStringList(object.value(QLatin1String("participants")), 256, 128)) {
        return fail(QStringLiteral("config.participants phải là danh sách chuỗi "
                                   "(tối đa 256 tên, mỗi tên 128 ký tự)"));
    }
    if (object.contains(QLatin1String("session_title"))
        && object.value(QLatin1String("session_title")).toString().size() > 200) {
        return fail(QStringLiteral("config.session_title quá dài (tối đa 200 ký tự)"));
    }
    const QString mode = object.value(QLatin1String("mode")).toString(
        QStringLiteral("record_and_s2t"));
    if (mode != QLatin1String("record_and_s2t") && mode != QLatin1String("record_only"))
        return fail(QStringLiteral("config.mode phải là 'record_and_s2t' hoặc 'record_only'"));
    const QString security = object.value(QLatin1String("security_level")).toString();
    if (!security.isEmpty() && security != QLatin1String("thuong")
        && security != QLatin1String("mat") && security != QLatin1String("toi_mat")
        && security != QLatin1String("tuyet_mat")) {
        return fail(QStringLiteral("config.security_level phải là một trong "
                                   "thuong/mat/toi_mat/tuyet_mat"));
    }
    return true;
}
