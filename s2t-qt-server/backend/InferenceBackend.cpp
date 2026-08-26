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
