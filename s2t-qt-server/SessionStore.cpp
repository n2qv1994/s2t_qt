#include "SessionStore.h"

#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// Longest range get_audio_range will serve.  The same bound the Python store
// used, and for the same reason: a reviewer needs a sentence, and an unbounded
// range turns one RPC into a whole meeting over the wire.
const double kMaxRangeSec = 60.0;

// How many trace events a session keeps PER STAGE.  A shared ceiling is spent
// by whichever stage talks most, so the stages an operator actually debugs
// punctuation with - correction_asr and itn - were the first to be evicted.
// Per stage, 2000 events is hours of correction windows.
const int kTraceKeepPerStage = 2000;

// A session id is used as a file name, so it has to be one.  Ids are minted by
// this server as hex, but a recovered journal or a hand-written test can carry
// anything, and a '/' in there would write outside the audio directory.
QString safeHandle(const QString &sessionId)
{
    QString out;
    out.reserve(sessionId.size());
    for (const QChar ch : sessionId) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            out.append(ch);
        else
            out.append(QLatin1Char('_'));
    }
    if (out.isEmpty())
        out = QStringLiteral("unnamed");
    return out;
}

// The three pass-through fields of SessionSummary, read back out of the
// config the meeting was started with.  They mean nothing to this server -
// security_level is a label, not an access control - but a picker showing a
// meeting has to show what the operator typed, and a second RPC for it is
// what the .proto put them here to avoid.
void applyConfigJson(const QString &configJson, asr::SessionSummary *summary)
{
    if (configJson.trimmed().isEmpty())
        return;
    const QJsonObject object = QJsonDocument::fromJson(configJson.toUtf8()).object();
    for (const QJsonValue &value : object.value(QStringLiteral("participants")).toArray())
        summary->participants.append(value.toString());
    summary->securityLevel = object.value(QStringLiteral("security_level")).toString();
    summary->mode = object.value(QStringLiteral("mode"))
                        .toString(QStringLiteral("record_and_s2t"));
    if (summary->title.isEmpty())
        summary->title = object.value(QStringLiteral("session_title")).toString();
}

QString spansToJson(const QList<QPair<double, double>> &spans)
{
    QJsonArray array;
    for (const auto &span : spans) {
        QJsonArray pair;
        pair.append(span.first);
        pair.append(span.second);
        array.append(pair);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<QPair<double, double>> spansFromJson(const QString &json)
{
    QList<QPair<double, double>> out;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    for (const QJsonValue &value : document.array()) {
        const QJsonArray pair = value.toArray();
        if (pair.size() >= 2)
            out.append({pair.at(0).toDouble(), pair.at(1).toDouble()});
    }
    return out;
}

// Named `values`, not `slots`: Qt #defines `slots` as a keyword, so a parameter
// by that name is a parse error in any translation unit that includes QObject.
QString slotsToJson(const QList<QString> &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<QString> slotsFromJson(const QString &json)
{
    QList<QString> out;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array())
        out.append(value.toString());
    return out;
}

// Binds a string to a TEXT NOT NULL column.
//
// A default-constructed QString is *null*, not empty, and QSqlQuery binds that
// as SQL NULL - which every TEXT NOT NULL column here rejects.  The values that
// reach this store are routinely absent: a meeting with no title, a recovered
// session with no client, a speaker with no publish error.  Without this, the
// insert fails and the meeting is silently never archived.
void bindText(QSqlQuery &query, const QString &value)
{
    query.addBindValue(value.isNull() ? QString::fromLatin1("") : value);
}

} // namespace

SessionStore::SessionStore()
    : m_connectionName(QStringLiteral("s2t-store-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)))
{
}

SessionStore::~SessionStore()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool SessionStore::open(const QString &dir, QString *error)
{
    m_dir = dir.trimmed();
    if (m_dir.isEmpty()) {
        LOG_INFO(applog::cat::Session)
            << "session store disabled (database/dir trống) - cuộc họp không được lưu lại";
        return true;
    }

    QDir root(m_dir);
    if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("không tạo được thư mục kho phiên '%1'").arg(m_dir);
        return false;
    }
    if (!QDir(root.filePath(QStringLiteral("audio"))).exists()
        && !root.mkpath(QStringLiteral("audio"))) {
        *error = QStringLiteral("không tạo được thư mục audio trong '%1'").arg(m_dir);
        return false;
    }

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        // Worth naming precisely: the Qt install is missing a plugin, which is
        // a deployment problem, not a configuration one.
        *error = QStringLiteral("Qt không có driver QSQLITE - thiếu plugin sqldrivers/qsqlite");
        return false;
    }
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(root.filePath(QStringLiteral("sessions.db")));
    if (!m_db.open()) {
        *error = QStringLiteral("không mở được kho phiên: %1").arg(m_db.lastError().text());
        return false;
    }

    QSqlQuery pragma(m_db);
    // WAL so a reader is never blocked by the writer, and NORMAL because the
    // durable copy of the audio is the journal and the flat file - losing the
    // last few metadata writes to a power cut costs a counter, not a meeting.
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    if (!migrate(error)) {
        m_db.close();
        return false;
    }
    m_enabled = true;
    LOG_INFO(applog::cat::Session) << "session store open at" << m_db.databaseName();
    return true;
}

bool SessionStore::migrate(QString *error)
{
    const char *const schema[] = {
        "CREATE TABLE IF NOT EXISTS sessions ("
        " session_id TEXT PRIMARY KEY,"
        " title TEXT NOT NULL DEFAULT '',"
        " client TEXT NOT NULL DEFAULT '',"
        " config_json TEXT NOT NULL DEFAULT '',"
        " created_at REAL NOT NULL DEFAULT 0,"
        " updated_at REAL NOT NULL DEFAULT 0,"
        " duration_sec REAL NOT NULL DEFAULT 0,"
        " final INTEGER NOT NULL DEFAULT 0,"
        " audio_samples INTEGER NOT NULL DEFAULT 0,"
        " sample_rate INTEGER NOT NULL DEFAULT 16000,"
        " channels INTEGER NOT NULL DEFAULT 1)",

        // The whole StateResponse, serialized with the same codec the wire
        // uses.  Storing it as a blob rather than as columns means a change to
        // SessionState needs no migration here - shared/proto is the one place
        // that knows its shape.
        "CREATE TABLE IF NOT EXISTS transcripts ("
        " session_id TEXT PRIMARY KEY,"
        " revision INTEGER NOT NULL DEFAULT 0,"
        " final INTEGER NOT NULL DEFAULT 0,"
        " commit_boundary_sec REAL NOT NULL DEFAULT 0,"
        " updated_at REAL NOT NULL DEFAULT 0,"
        " state BLOB)",

        "CREATE TABLE IF NOT EXISTS audit ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " session_id TEXT NOT NULL,"
        " ts REAL NOT NULL,"
        " event TEXT NOT NULL,"
        " payload_json TEXT NOT NULL DEFAULT '')",
        "CREATE INDEX IF NOT EXISTS audit_by_session ON audit(session_id, id)",

        "CREATE TABLE IF NOT EXISTS session_speakers ("
        " session_id TEXT NOT NULL,"
        " session_speaker_id TEXT NOT NULL,"
        " diar_slots TEXT NOT NULL DEFAULT '[]',"
        " verified_name TEXT NOT NULL DEFAULT '',"
        // "model" or "manual" - a manual rename must survive a later sync.
        " verified_name_source TEXT NOT NULL DEFAULT 'model',"
        " score REAL NOT NULL DEFAULT 0,"
        " windows INTEGER NOT NULL DEFAULT 0,"
        " created_at REAL NOT NULL DEFAULT 0,"
        " updated_at REAL NOT NULL DEFAULT 0,"
        " status TEXT NOT NULL DEFAULT 'pending',"
        " published_name TEXT NOT NULL DEFAULT '',"
        " published_at REAL NOT NULL DEFAULT 0,"
        " publish_error TEXT NOT NULL DEFAULT '',"
        " evidence_json TEXT NOT NULL DEFAULT '',"
        " evidence_speech_sec REAL NOT NULL DEFAULT 0,"
        " evidence_staged_at REAL NOT NULL DEFAULT 0,"
        " evidence_source_name TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY (session_id, session_speaker_id))",

        // What get_pipeline_trace answers with.  Its own table and not the
        // audit one: audit records decisions a person made and is kept
        // forever, while this records what the tier did on one chunk, is
        // written several times a second, and is trimmed behind the caller's
        // back.  Mixing the two would make the operator's own history
        // unreadable within a minute of recording.
        "CREATE TABLE IF NOT EXISTS pipeline_trace ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " session_id TEXT NOT NULL,"
        " seq INTEGER NOT NULL DEFAULT 0,"
        " ts REAL NOT NULL DEFAULT 0,"
        " stage TEXT NOT NULL DEFAULT '',"
        " event TEXT NOT NULL DEFAULT '',"
        " audio_start_sec REAL NOT NULL DEFAULT 0,"
        " audio_end_sec REAL NOT NULL DEFAULT 0,"
        " payload_json TEXT NOT NULL DEFAULT '')",
        "CREATE INDEX IF NOT EXISTS trace_by_session ON pipeline_trace(session_id, seq)",
    };

    QSqlQuery query(m_db);
    for (const char *statement : schema) {
        if (!query.exec(QString::fromLatin1(statement))) {
            *error = QStringLiteral("không tạo được lược đồ kho phiên: %1")
                         .arg(query.lastError().text());
            return false;
        }
    }
    return true;
}

QString SessionStore::audioPath(const QString &sessionId) const
{
    return QDir(m_dir).filePath(QStringLiteral("audio/%1.s16le").arg(safeHandle(sessionId)));
}

// ---- the meeting -----------------------------------------------------------

void SessionStore::createSession(const QString &sessionId, const QString &title,
                                 const QString &client, const QString &configJson,
                                 quint32 sampleRate, quint32 channels)
{
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO sessions (session_id, title, client, config_json, created_at, updated_at,"
        " sample_rate, channels) VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(session_id) DO UPDATE SET title=excluded.title, updated_at=excluded.updated_at"));
    const double now = nowSeconds();
    bindText(query, sessionId);
    bindText(query, title);
    bindText(query, client);
    bindText(query, configJson);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(int(sampleRate ? sampleRate : 16000));
    query.addBindValue(int(channels ? channels : 1));
    if (!query.exec()) {
        LOG_WARN(applog::cat::Session)
            << "không ghi được phiên" << sessionId << "-" << query.lastError().text();
    }
}

void SessionStore::saveState(const QString &sessionId, const asr::StateResponse &state)
{
    if (!m_enabled)
        return;
    const QByteArray blob = state.serialize();
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO transcripts (session_id, revision, final, commit_boundary_sec, updated_at,"
        " state) VALUES (?,?,?,?,?,?)"
        " ON CONFLICT(session_id) DO UPDATE SET revision=excluded.revision, final=excluded.final,"
        " commit_boundary_sec=excluded.commit_boundary_sec, updated_at=excluded.updated_at,"
        " state=excluded.state"));
    bindText(query, sessionId);
    query.addBindValue(qulonglong(state.transcriptRevision));
    query.addBindValue(state.transcriptFinal ? 1 : 0);
    query.addBindValue(state.commitBoundarySec);
    query.addBindValue(nowSeconds());
    query.addBindValue(blob);
    if (!query.exec()) {
        LOG_WARN(applog::cat::Session)
            << "không lưu được bản chép" << sessionId << "-" << query.lastError().text();
    }
}

bool SessionStore::loadState(const QString &sessionId, asr::StateResponse *out)
{
    if (!m_enabled)
        return false;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT state FROM transcripts WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec() || !query.next())
        return false;
    const QByteArray blob = query.value(0).toByteArray();
    if (blob.isEmpty())
        return false;
    pw::Reader reader(blob);
    out->parse(reader);
    if (!reader.ok()) {
        // A blob that will not decode is a corrupt row, not an empty meeting.
        // Saying so beats handing back a transcript with no words in it.
        LOG_ERROR(applog::cat::Session)
            << "bản chép đã lưu của" << sessionId << "không giải mã được - bỏ qua";
        return false;
    }
    return true;
}

void SessionStore::markFinished(const QString &sessionId, double durationSec)
{
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE sessions SET final = 1, duration_sec = ?, updated_at = ? WHERE session_id = ?"));
    query.addBindValue(durationSec);
    query.addBindValue(nowSeconds());
    query.addBindValue(sessionId);
    query.exec();
}

bool SessionStore::hasSession(const QString &sessionId)
{
    if (!m_enabled)
        return false;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE session_id = ?"));
    query.addBindValue(sessionId);
    return query.exec() && query.next();
}

QList<asr::SessionSummary> SessionStore::listSessions(int limit, const QString &cursor,
                                                      QString *nextCursor)
{
    QList<asr::SessionSummary> out;
    if (nextCursor)
        nextCursor->clear();
    if (!m_enabled)
        return out;

    const int wanted = limit > 0 ? qMin(limit, 500) : 50;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);

    // The cursor is (created_at, session_id) of the LAST row handed out, and
    // the next page starts strictly after it in that order.
    //
    // It used to be the created_at of the first row of the *next* page,
    // compared with `<`, which skipped that row outright: every page boundary
    // silently lost one meeting.  Measured 2026-09-24 on 23 sessions - pages
    // of two returned 16, pages of five returned 20 - so the history window
    // drops meetings exactly when an operator scrolls back to look for one.
    //
    // The session id is in the key because created_at is a millisecond
    // timestamp and two meetings started in the same millisecond would
    // otherwise be an infinite loop or a lost row, depending on which way the
    // comparison fell.
    const int split = cursor.indexOf(QLatin1Char('|'));
    const double cursorAt = split > 0 ? cursor.left(split).toDouble() : 0.0;
    const QString cursorId = split > 0 ? cursor.mid(split + 1) : QString();
    if (cursor.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT session_id, title, created_at, updated_at, duration_sec, final, config_json"
            " FROM sessions ORDER BY created_at DESC, session_id DESC LIMIT ?"));
        query.addBindValue(wanted + 1);
    } else {
        query.prepare(QStringLiteral(
            "SELECT session_id, title, created_at, updated_at, duration_sec, final, config_json"
            " FROM sessions WHERE created_at < ? OR (created_at = ? AND session_id < ?)"
            " ORDER BY created_at DESC, session_id DESC LIMIT ?"));
        query.addBindValue(cursorAt);
        query.addBindValue(cursorAt);
        query.addBindValue(cursorId);
        query.addBindValue(wanted + 1);
    }
    if (!query.exec()) {
        LOG_WARN(applog::cat::Session) << "list_sessions lỗi:" << query.lastError().text();
        return out;
    }
    bool more = false;
    while (query.next()) {
        if (out.size() >= wanted) {
            // One row past the page proves there IS a next page; the cursor
            // itself is the last row we are returning, so nothing is skipped.
            more = true;
            break;
        }
        asr::SessionSummary summary;
        summary.sessionId = query.value(0).toString();
        summary.title = query.value(1).toString();
        summary.createdAt = query.value(2).toDouble();
        summary.updatedAt = query.value(3).toDouble();
        summary.durationSec = query.value(4).toDouble();
        summary.final = query.value(5).toInt() != 0;
        summary.running = false; // a stored session is not the live one
        // What the operator entered, read back out of the config the meeting
        // was started with.  It was dropped on the floor here until
        // 2026-09-24, so a session picker showed empty participants and no
        // security label for every archived meeting.
        applyConfigJson(query.value(6).toString(), &summary);
        out.append(summary);
    }
    if (more && nextCursor && !out.isEmpty()) {
        *nextCursor = QString::number(out.last().createdAt, 'f', 6) + QLatin1Char('|')
            + out.last().sessionId;
    }
    return out;
}

// ---- audio -----------------------------------------------------------------

void SessionStore::appendAudio(const QString &sessionId, const QByteArray &pcm)
{
    if (!m_enabled || pcm.isEmpty())
        return;

    QFile file(audioPath(sessionId));
    if (!file.open(QIODevice::Append)) {
        LOG_WARN(applog::cat::Session)
            << "không mở được tệp audio cho" << sessionId << "-" << file.errorString();
        return;
    }
    const qint64 written = file.write(pcm);
    file.flush();
    file.close();
    if (written != pcm.size()) {
        // Short write: do NOT record the samples.  audioRange() clamps to the
        // recorded count, so understating it costs a reviewer the tail of a
        // range while overstating it reads past the end of the file.
        LOG_ERROR(applog::cat::Session)
            << "ghi thiếu audio cho" << sessionId << written << "/" << pcm.size() << "byte";
        return;
    }

    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE sessions SET audio_samples = audio_samples + ?, updated_at = ?"
        " WHERE session_id = ?"));
    query.addBindValue(qlonglong(pcm.size() / 2));
    query.addBindValue(nowSeconds());
    query.addBindValue(sessionId);
    query.exec();
}

bool SessionStore::audioFormat(const QString &sessionId, quint32 *sampleRate, quint32 *channels,
                               qint64 *samples)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT sample_rate, channels, audio_samples FROM sessions WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec() || !query.next())
        return false;
    *sampleRate = quint32(qMax(1, query.value(0).toInt()));
    *channels = quint32(qMax(1, query.value(1).toInt()));
    *samples = query.value(2).toLongLong();
    return true;
}

bool SessionStore::audioRange(const QString &sessionId, double startSec, double endSec,
                              asr::AudioRangeResponse *out, QString *error)
{
    if (!m_enabled) {
        *error = QStringLiteral("máy chủ chưa bật kho phiên (database/dir) nên không lưu audio");
        return false;
    }
    // Clamped, not refused.
    //
    // A reviewer dragging over the waveform routinely asks for a window that
    // starts before zero or runs past the end - that is what dragging to the
    // edge means - and answering INVALID_ARGUMENT there makes the player stop
    // dead at exactly the moment somebody is trying to hear the beginning or
    // the last sentence.  The response says which range it actually served, so
    // a clamp is self-describing in a way a refusal is not.  Only a genuinely
    // empty request - end at or before start - is still an error.
    if (endSec <= startSec) {
        *error = QStringLiteral("khoảng audio phải có start_sec < end_sec");
        return false;
    }
    if (startSec < 0.0)
        startSec = 0.0;
    if (endSec - startSec > kMaxRangeSec)
        endSec = startSec + kMaxRangeSec;

    quint32 sampleRate = 16000;
    quint32 channels = 1;
    qint64 totalSamples = 0;
    {
        QMutexLocker lock(&m_mutex);
        if (!audioFormat(sessionId, &sampleRate, &channels, &totalSamples)) {
            *error = QStringLiteral("kho phiên không có phiên '%1'").arg(sessionId);
            return false;
        }
    }

    out->sessionId = sessionId;
    out->sampleRate = sampleRate;
    out->channels = channels;
    out->audioFormat = QStringLiteral("s16le");
    out->totalSec = double(totalSamples) / double(sampleRate * channels);

    const qint64 frameRate = qint64(sampleRate) * qint64(channels);
    qint64 startSample = qBound<qint64>(0, qint64(startSec * double(frameRate) + 0.5), totalSamples);
    qint64 endSample = qBound<qint64>(startSample, qint64(endSec * double(frameRate) + 0.5),
                                      totalSamples);

    QFile file(audioPath(sessionId));
    if (!file.exists() || totalSamples == 0) {
        // A meeting that was started and never pushed to.  Empty, not missing.
        out->startSec = 0.0;
        out->endSec = 0.0;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("không đọc được audio đã lưu: %1").arg(file.errorString());
        return false;
    }
    if (!file.seek(startSample * 2)) {
        *error = QStringLiteral("không nhảy được tới vị trí audio yêu cầu");
        return false;
    }
    out->pcm = file.read((endSample - startSample) * 2);
    out->startSec = double(startSample) / double(frameRate);
    out->endSec = double(startSample + out->pcm.size() / 2) / double(frameRate);
    return true;
}

// ---- audit -----------------------------------------------------------------

void SessionStore::appendAudit(const QString &sessionId, const QString &event,
                               const QString &payloadJson)
{
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO audit (session_id, ts, event, payload_json) VALUES (?,?,?,?)"));
    bindText(query, sessionId);
    query.addBindValue(nowSeconds());
    bindText(query, event);
    bindText(query, payloadJson);
    query.exec();
}

QList<asr::AuditEvent> SessionStore::auditHistory(const QString &sessionId, int limit)
{
    QList<asr::AuditEvent> out;
    if (!m_enabled)
        return out;
    const int wanted = limit > 0 ? qMin(limit, 2000) : 200;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    // Newest first out of the database, then reversed: the client renders a
    // history oldest-first, and "the last 200 events" is the useful window.
    query.prepare(QStringLiteral(
        "SELECT ts, event, payload_json FROM audit WHERE session_id = ?"
        " ORDER BY id DESC LIMIT ?"));
    query.addBindValue(sessionId);
    query.addBindValue(wanted);
    if (!query.exec())
        return out;
    while (query.next()) {
        asr::AuditEvent item;
        item.ts = query.value(0).toDouble();
        item.event = query.value(1).toString();
        item.payloadJson = query.value(2).toString();
        out.prepend(item);
    }
    return out;
}

// ---- the pipeline trace ----------------------------------------------------

void SessionStore::appendTrace(const QString &sessionId,
                               const QList<asr::PipelineTraceEvent> &events)
{
    if (!m_enabled || events.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    m_db.transaction();
    for (const asr::PipelineTraceEvent &event : events) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO pipeline_trace (session_id, seq, ts, stage, event, audio_start_sec,"
            " audio_end_sec, payload_json) VALUES (?,?,?,?,?,?,?,?)"));
        bindText(query, sessionId);
        query.addBindValue(qulonglong(event.seq));
        query.addBindValue(event.ts);
        bindText(query, event.stage);
        bindText(query, event.event);
        query.addBindValue(event.audioStartSec);
        query.addBindValue(event.audioEndSec);
        bindText(query, event.payloadJson);
        query.exec();
    }
    m_db.commit();

    // Trimmed here rather than on a timer: this is the only writer, so it is
    // the only place that knows the table has grown.  A three-hour meeting
    // produces tens of thousands of these and nobody reads past the last few
    // hundred - what a trace is for is "what did the tier decide just now".
    //
    // Per STAGE, not per session.  One shared ceiling is spent by whichever
    // stage is noisiest: `vad` and `asr` fire several times a second while
    // `correction_asr` and `itn` fire once a window, so after about twenty
    // minutes the only two stages anybody debugs punctuation with had been
    // pushed out of the table by the two nobody reads.
    QSet<QString> stages;
    for (const asr::PipelineTraceEvent &event : events)
        stages.insert(event.stage);
    for (const QString &stage : stages) {
        QSqlQuery trim(m_db);
        trim.prepare(QStringLiteral(
            "DELETE FROM pipeline_trace WHERE session_id = ? AND stage = ? AND id NOT IN"
            " (SELECT id FROM pipeline_trace WHERE session_id = ? AND stage = ?"
            "  ORDER BY id DESC LIMIT ?)"));
        bindText(trim, sessionId);
        bindText(trim, stage);
        bindText(trim, sessionId);
        bindText(trim, stage);
        trim.addBindValue(kTraceKeepPerStage);
        trim.exec();
    }
}

QList<asr::PipelineTraceEvent> SessionStore::traceHistory(const QString &sessionId,
                                                          quint64 afterSeq, int limit,
                                                          const QList<QString> &stages,
                                                          bool *hasMore)
{
    QList<asr::PipelineTraceEvent> out;
    if (hasMore)
        *hasMore = false;
    if (!m_enabled)
        return out;
    const int wanted = limit > 0 ? qMin(limit, 2000) : 200;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    // Oldest first from `after_seq`, which is what makes the request a cursor:
    // a client polls with the nextSeq it was last given and gets only what has
    // happened since.  One row past the limit is read so has_more is an
    // observation rather than a guess.
    query.prepare(QStringLiteral(
        "SELECT seq, ts, stage, event, audio_start_sec, audio_end_sec, payload_json"
        " FROM pipeline_trace WHERE session_id = ? AND seq > ? ORDER BY seq ASC LIMIT ?"));
    bindText(query, sessionId);
    query.addBindValue(qulonglong(afterSeq));
    query.addBindValue(wanted + 1);
    if (!query.exec())
        return out;
    while (query.next()) {
        asr::PipelineTraceEvent item;
        item.seq = query.value(0).toULongLong();
        item.ts = query.value(1).toDouble();
        item.stage = query.value(2).toString();
        item.event = query.value(3).toString();
        item.audioStartSec = query.value(4).toDouble();
        item.audioEndSec = query.value(5).toDouble();
        item.payloadJson = query.value(6).toString();
        // Filtering here and not in SQL: the stage list is caller-supplied and
        // an empty one means "every stage", which is the common case.
        if (!stages.isEmpty() && !stages.contains(item.stage))
            continue;
        if (out.size() >= wanted) {
            if (hasMore)
                *hasMore = true;
            break;
        }
        out.append(item);
    }
    return out;
}

// ---- the per-session speaker registry --------------------------------------

void SessionStore::syncSpeakers(const QString &sessionId,
                                const QList<reg::SessionSpeakerEntry> &entries)
{
    if (!m_enabled || entries.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    m_db.transaction();
    for (const reg::SessionSpeakerEntry &entry : entries) {
        QSqlQuery query(m_db);
        // Identity and metadata only.  status/published_*/evidence belong to
        // the publish decision and are never touched here - and verified_name
        // is only taken when nobody has renamed this speaker by hand, because
        // the tier's automatic answer knows nothing about that correction and
        // would silently undo a rename that already succeeded.
        query.prepare(QStringLiteral(
            "INSERT INTO session_speakers (session_id, session_speaker_id, diar_slots,"
            " verified_name, score, windows, created_at, updated_at)"
            " VALUES (?,?,?,?,?,?,?,?)"
            " ON CONFLICT(session_id, session_speaker_id) DO UPDATE SET"
            "  diar_slots=excluded.diar_slots, score=excluded.score, windows=excluded.windows,"
            "  updated_at=excluded.updated_at,"
            "  verified_name=CASE WHEN session_speakers.verified_name_source='manual'"
            "                     THEN session_speakers.verified_name"
            "                     ELSE excluded.verified_name END"));
        const double now = nowSeconds();
        bindText(query, sessionId);
        bindText(query, entry.sessionSpeakerId);
        bindText(query, slotsToJson(entry.diarSlots));
        bindText(query, entry.verifiedName);
        query.addBindValue(entry.score);
        query.addBindValue(int(entry.windows));
        query.addBindValue(entry.createdAt > 0.0 ? entry.createdAt : now);
        query.addBindValue(now);
        if (!query.exec()) {
            LOG_WARN(applog::cat::Session)
                << "sync speaker" << entry.sessionSpeakerId << "lỗi:" << query.lastError().text();
        }
    }
    m_db.commit();
}

QList<reg::SessionSpeakerEntry> SessionStore::listSpeakers(const QString &sessionId)
{
    QList<reg::SessionSpeakerEntry> out;
    if (!m_enabled)
        return out;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT session_speaker_id, diar_slots, verified_name, score, windows, created_at,"
        " updated_at, status, published_name, published_at, publish_error, evidence_json,"
        " evidence_speech_sec, evidence_staged_at, evidence_source_name"
        " FROM session_speakers WHERE session_id = ? ORDER BY session_speaker_id"));
    query.addBindValue(sessionId);
    if (!query.exec())
        return out;
    while (query.next()) {
        reg::SessionSpeakerEntry entry;
        entry.sessionSpeakerId = query.value(0).toString();
        entry.diarSlots = slotsFromJson(query.value(1).toString());
        entry.verifiedName = query.value(2).toString();
        entry.score = query.value(3).toDouble();
        entry.windows = quint32(query.value(4).toInt());
        entry.createdAt = query.value(5).toDouble();
        entry.updatedAt = query.value(6).toDouble();
        entry.status = query.value(7).toString();
        entry.publishedName = query.value(8).toString();
        entry.publishedAt = query.value(9).toDouble();
        entry.publishError = query.value(10).toString();
        const QString evidence = query.value(11).toString();
        if (!evidence.isEmpty() && evidence != QLatin1String("[]")) {
            entry.hasEvidence = true;
            entry.evidence.spanCount = quint32(spansFromJson(evidence).size());
            entry.evidence.totalSpeechSec = query.value(12).toDouble();
            entry.evidence.stagedAt = query.value(13).toDouble();
            entry.evidence.sourceVerifiedName = query.value(14).toString();
        }
        out.append(entry);
    }
    return out;
}

bool SessionStore::speaker(const QString &sessionId, const QString &speakerId,
                           reg::SessionSpeakerEntry *out)
{
    for (const reg::SessionSpeakerEntry &entry : listSpeakers(sessionId)) {
        if (entry.sessionSpeakerId == speakerId) {
            *out = entry;
            return true;
        }
    }
    return false;
}

void SessionStore::setVerifiedName(const QString &sessionId, const QString &speakerId,
                                   const QString &name, bool manual)
{
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO session_speakers (session_id, session_speaker_id, verified_name,"
        " verified_name_source, created_at, updated_at) VALUES (?,?,?,?,?,?)"
        " ON CONFLICT(session_id, session_speaker_id) DO UPDATE SET"
        "  verified_name=excluded.verified_name,"
        "  verified_name_source=excluded.verified_name_source,"
        "  updated_at=excluded.updated_at"));
    const double now = nowSeconds();
    bindText(query, sessionId);
    bindText(query, speakerId);
    bindText(query, name);
    query.addBindValue(manual ? QStringLiteral("manual") : QStringLiteral("model"));
    query.addBindValue(now);
    query.addBindValue(now);
    query.exec();
}

void SessionStore::stageEvidence(const QString &sessionId, const QString &speakerId,
                                 const QList<QPair<double, double>> &spans,
                                 const QString &sourceName)
{
    if (!m_enabled)
        return;
    double speech = 0.0;
    for (const auto &span : spans)
        speech += qMax(0.0, span.second - span.first);

    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO session_speakers (session_id, session_speaker_id, evidence_json,"
        " evidence_speech_sec, evidence_staged_at, evidence_source_name, created_at, updated_at)"
        " VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(session_id, session_speaker_id) DO UPDATE SET"
        "  evidence_json=excluded.evidence_json,"
        "  evidence_speech_sec=excluded.evidence_speech_sec,"
        "  evidence_staged_at=excluded.evidence_staged_at,"
        "  evidence_source_name=excluded.evidence_source_name,"
        "  updated_at=excluded.updated_at"));
    const double now = nowSeconds();
    bindText(query, sessionId);
    bindText(query, speakerId);
    bindText(query, spansToJson(spans));
    query.addBindValue(speech);
    query.addBindValue(now);
    bindText(query, sourceName);
    query.addBindValue(now);
    query.addBindValue(now);
    query.exec();
}

QList<QPair<double, double>> SessionStore::evidenceSpans(const QString &sessionId,
                                                         const QString &speakerId)
{
    QList<QPair<double, double>> out;
    if (!m_enabled)
        return out;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT evidence_json FROM session_speakers"
        " WHERE session_id = ? AND session_speaker_id = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(speakerId);
    if (query.exec() && query.next())
        out = spansFromJson(query.value(0).toString());
    return out;
}

void SessionStore::updateSpeakerStatus(const QString &sessionId, const QString &speakerId,
                                       const QString &status, const QString &publishedName,
                                       const QString &publishError)
{
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE session_speakers SET status = ?, published_name = ?, published_at = ?,"
        " publish_error = ?, updated_at = ?"
        " WHERE session_id = ? AND session_speaker_id = ?"));
    bindText(query, status);
    bindText(query, publishedName);
    query.addBindValue(publishedName.trimmed().isEmpty() ? 0.0 : nowSeconds());
    bindText(query, publishError);
    query.addBindValue(nowSeconds());
    bindText(query, sessionId);
    bindText(query, speakerId);
    query.exec();
}

QString SessionStore::speakerForSlot(const QString &sessionId, const QString &diarSlot)
{
    if (!m_enabled || diarSlot.isEmpty())
        return QString();
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT session_speaker_id, diar_slots FROM session_speakers WHERE session_id = ?"));
    query.addBindValue(sessionId);
    if (!query.exec())
        return QString();
    while (query.next()) {
        // Not named `slots`: Qt #defines that as a keyword.
        const QList<QString> owned = slotsFromJson(query.value(1).toString());
        if (owned.contains(diarSlot))
            return query.value(0).toString();
    }
    return QString();
}

void SessionStore::speakerCounts(const QString &sessionId, quint32 *pending, quint32 *published,
                                 quint32 *failed)
{
    *pending = 0;
    *published = 0;
    *failed = 0;
    if (!m_enabled)
        return;
    QMutexLocker lock(&m_mutex);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT status, COUNT(*) FROM session_speakers WHERE session_id = ? GROUP BY status"));
    query.addBindValue(sessionId);
    if (!query.exec())
        return;
    while (query.next()) {
        const QString status = query.value(0).toString();
        const quint32 count = quint32(qMax(0, query.value(1).toInt()));
        // `global_shared` IS published - it is the status the publish path
        // writes on success.  Counting only a literal "published" is why a
        // voice that had just been written to the shared database was still
        // reported as pending.
        if (status == QLatin1String("published") || status == QLatin1String("global_shared"))
            *published += count;
        else if (status == QLatin1String("publish_failed"))
            *failed += count;
        else
            *pending += count;
    }
}

bool SessionStore::deleteSession(const QString &sessionId, quint64 *bytesRemoved, QString *error)
{
    if (bytesRemoved)
        *bytesRemoved = 0;
    if (!m_enabled) {
        *error = QStringLiteral("máy chủ này không có kho phiên (database/dir trống)");
        return false;
    }
    {
        QMutexLocker lock(&m_mutex);
        QSqlQuery exists(m_db);
        exists.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE session_id = ?"));
        exists.addBindValue(sessionId);
        if (!exists.exec() || !exists.next()) {
            *error = QStringLiteral("kho phiên không có phiên '%1'").arg(sessionId);
            return false;
        }
    }

    QFile audio(audioPath(sessionId));
    quint64 removed = 0;
    if (audio.exists()) {
        removed = quint64(audio.size());
        if (!audio.remove()) {
            *error = QStringLiteral("không xoá được tệp audio của phiên '%1': %2")
                         .arg(sessionId, audio.errorString());
            return false;
        }
    }

    QMutexLocker lock(&m_mutex);
    m_db.transaction();
    for (const char *statement : {"DELETE FROM sessions WHERE session_id = ?",
                                  "DELETE FROM transcripts WHERE session_id = ?",
                                  "DELETE FROM session_speakers WHERE session_id = ?",
                                  "DELETE FROM pipeline_trace WHERE session_id = ?"}) {
        QSqlQuery query(m_db);
        query.prepare(QString::fromLatin1(statement));
        query.addBindValue(sessionId);
        query.exec();
    }
    m_db.commit();
    if (bytesRemoved)
        *bytesRemoved = removed;
    LOG_INFO(applog::cat::Session)
        << "session" << sessionId << "deleted from the archive -" << removed << "bytes of audio";
    return true;
}
