#include "LiveTranscript.h"

#include <QDateTime>
#include <QHash>

#include <algorithm>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// A gap longer than this between two words from the same speaker still starts a
// new row.  Without it a speaker who talks for an hour produces one row an hour
// long, which the client renders as a single unscrollable block.
const double kTurnGapSec = 1.5;

// Below this the client paints a word as low confidence.  It is reported to the
// client as conf_threshold_pct so both sides agree on what "low" means.
const quint32 kConfThresholdPct = 60;

// Bound on the amplitude strip.  At one sample per 160 ms packet this is about
// two and a half hours; past that the oldest samples are dropped and the step
// widens, so the strip still spans the whole meeting.
const int kAmpTraceMax = 4096;

bool isLowConfidence(float confidence)
{
    return confidence > 0.0f && confidence < float(kConfThresholdPct) / 100.0f;
}

} // namespace

void LiveTranscript::configure(const QString &title, quint32 sampleRate, quint32 channels)
{
    m_title = title;
    m_sampleRate = qMax(1u, sampleRate);
    m_channels = qMax(1u, channels);
    m_startedAt = nowSeconds();
}

quint64 LiveTranscript::apply(const asr::PushAudioResponse &response)
{
    ++m_version;

    if (response.sourceSeenSec > m_sourceSeenSec)
        m_sourceSeenSec = response.sourceSeenSec;
    if (response.speechSeenSec > m_speechSeenSec)
        m_speechSeenSec = response.speechSeenSec;
    if (response.correction.commitBoundarySec > m_commitBoundarySec)
        m_commitBoundarySec = response.correction.commitBoundarySec;
    if (response.chunkEndSec > 0.0 && response.chunkStartSec >= 0.0)
        m_lastChunkMs = (response.chunkEndSec - response.chunkStartSec) * 1000.0;

    // The amplitude strip is drawn from confidence when the tier gives no
    // level: it is not loudness, but it moves with speech and stays flat in
    // silence, which is what the strip is read for.
    if (m_ampTrace.size() >= kAmpTraceMax) {
        // Halve in place rather than drop the head: the strip keeps spanning
        // the whole meeting, at half the resolution.
        QList<float> thinned;
        thinned.reserve(m_ampTrace.size() / 2 + 1);
        for (int i = 0; i < m_ampTrace.size(); i += 2)
            thinned.append(m_ampTrace.at(i));
        m_ampTrace = thinned;
        m_ampStepSec *= 2.0;
    }
    m_ampTrace.append(response.asrConfidence);
    if (m_ampStepSec <= 0.0 && response.chunkEndSec > response.chunkStartSec)
        m_ampStepSec = response.chunkEndSec - response.chunkStartSec;

    // A correction pass rewrites words the tier had already emitted.  When one
    // arrives it is authoritative for its own range, so the range is dropped
    // and rebuilt rather than appended to - appending is what produces the
    // duplicated half-sentences this project has fought before.
    if (!response.correction.mergedWords.isEmpty()) {
        const double from = response.correction.updateStartSec;
        const double to = response.correction.updateEndSec > from ? response.correction.updateEndSec
                                                                  : m_sourceSeenSec;
        applyEdit(m_revision, from, to, response.correction.mergedWords);
    }

    if (!response.asrWords.isEmpty()) {
        appendWords(response.asrWords, response.speaker, response.speakerProb,
                    response.verifiedName);
        ++m_revision;
    } else if (!response.text.isEmpty()) {
        // A tier that gives no word timings still gives text.  It becomes one
        // word spanning the chunk, so the row machinery and the editor both
        // keep working rather than special-casing a timing-less backend.
        asr::Word whole;
        whole.w = response.text;
        whole.c = response.asrConfidence;
        whole.startSec = response.chunkStartSec;
        whole.endSec = response.chunkEndSec > response.chunkStartSec ? response.chunkEndSec
                                                                     : response.chunkStartSec;
        whole.speaker = response.speaker;
        appendWords({whole}, response.speaker, response.speakerProb, response.verifiedName);
        ++m_revision;
    }

    // The interim edge.  Replaced wholesale every time, never appended.
    if (!response.streamingText.isEmpty()) {
        m_provisional = asr::DisplayRow();
        m_provisional.rowId = QStringLiteral("prov");
        m_provisional.speaker = response.speaker;
        m_provisional.speakerProb = response.speakerProb;
        m_provisional.verifiedName = response.verifiedName;
        m_provisional.startSec = response.chunkStartSec;
        m_provisional.endSec = response.chunkEndSec;
        m_provisional.updatingText = response.streamingText;
        m_provisional.mergedText = response.streamingText;
        m_provisional.isProvisional = true;
        m_haveProvisional = true;
    } else if (!response.asrWords.isEmpty()) {
        // Words landed and nothing interim came with them: the edge has been
        // committed, so clearing it is what stops the same text showing twice.
        m_haveProvisional = false;
        m_provisional = asr::DisplayRow();
    }

    recount();
    return m_version;
}

void LiveTranscript::appendWords(const QList<asr::Word> &words, const QString &speaker,
                                 float speakerProb, const QString &verifiedName)
{
    for (const asr::Word &incoming : words) {
        asr::Word word = incoming;
        // The tier may tag words individually or only per chunk; a per-word tag
        // wins, because diarization inside a chunk is the finer answer.
        if (word.speaker.isEmpty())
            word.speaker = speaker;

        const bool sameTurn = !m_rows.isEmpty() && m_rows.last().speaker == word.speaker
            && (word.startSec - m_rows.last().endSec) <= kTurnGapSec;
        if (!sameTurn) {
            asr::DisplayRow row;
            row.rowId = QStringLiteral("r%1").arg(m_nextRowId++);
            row.speaker = word.speaker;
            row.speakerProb = speakerProb;
            row.verifiedName = verifiedName;
            row.startSec = word.startSec;
            row.endSec = word.endSec;
            m_rows.append(row);
        }

        asr::DisplayRow &row = m_rows.last();
        row.displayTokens.append(word);
        row.endSec = qMax(row.endSec, word.endSec);
        if (!verifiedName.isEmpty())
            row.verifiedName = verifiedName;
        if (speakerProb > 0.0f)
            row.speakerProb = speakerProb;
    }

    if (!m_rows.isEmpty())
        rebuildPhrases(&m_rows.last());
}

void LiveTranscript::rebuildPhrases(asr::DisplayRow *row) const
{
    row->phrases.clear();
    row->mergedText.clear();
    row->stableTokenCount = quint32(row->displayTokens.size());

    // One phrase per sentence, split on end punctuation.  The tier's
    // punctuation model is what makes this meaningful; without punctuation the
    // whole row is one phrase, which is the honest answer.
    asr::Phrase phrase;
    float sum = 0.0f;
    int count = 0;
    for (const asr::Word &word : row->displayTokens) {
        if (phrase.words.isEmpty())
            phrase.startSec = word.startSec;
        phrase.words.append(word);
        phrase.endSec = word.endSec;
        sum += word.c;
        ++count;
        if (!phrase.text.isEmpty())
            phrase.text += QLatin1Char(' ');
        phrase.text += word.w;

        const QChar last = word.w.isEmpty() ? QChar() : word.w.at(word.w.size() - 1);
        const bool endsSentence = last == QLatin1Char('.') || last == QLatin1Char('?')
            || last == QLatin1Char('!') || last == QLatin1Char(u'…');
        if (endsSentence) {
            phrase.avgConf = count > 0 ? sum / float(count) : 0.0f;
            phrase.isLowConf = isLowConfidence(phrase.avgConf);
            row->phrases.append(phrase);
            phrase = asr::Phrase();
            sum = 0.0f;
            count = 0;
        }
    }
    if (!phrase.words.isEmpty()) {
        phrase.avgConf = count > 0 ? sum / float(count) : 0.0f;
        phrase.isLowConf = isLowConfidence(phrase.avgConf);
        row->phrases.append(phrase);
    }

    for (const asr::Phrase &item : row->phrases) {
        if (!row->mergedText.isEmpty())
            row->mergedText += QLatin1Char(' ');
        row->mergedText += item.text;
    }
}

void LiveTranscript::recount()
{
    m_nPhrases = 0;
    m_nLow = 0;
    for (const asr::DisplayRow &row : m_rows) {
        m_nPhrases += quint32(row.phrases.size());
        for (const asr::Phrase &phrase : row.phrases) {
            if (phrase.isLowConf)
                ++m_nLow;
        }
    }
}

void LiveTranscript::markDone()
{
    if (m_haveProvisional && !m_provisional.displayTokens.isEmpty()) {
        // Whatever the edge held is now as settled as it will ever be.
        m_provisional.isProvisional = false;
        m_rows.append(m_provisional);
        rebuildPhrases(&m_rows.last());
    }
    m_haveProvisional = false;
    m_provisional = asr::DisplayRow();
    m_done = true;
    ++m_version;
    recount();
}

QList<QString> LiveTranscript::speakerIds() const
{
    QList<QString> out;
    for (const asr::DisplayRow &row : m_rows) {
        if (!row.speaker.isEmpty() && !out.contains(row.speaker))
            out.append(row.speaker);
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.toInt() < b.toInt();
    });
    return out;
}

asr::StateResponse LiveTranscript::snapshot(const QString &sessionId, qint64 streamId,
                                            double viewStartSec, double viewEndSec) const
{
    asr::StateResponse out;
    out.sessionId = sessionId;
    out.streamId = streamId;
    out.stateVersion = m_version;
    out.transcriptRevision = m_revision;
    out.transcriptFinal = m_done;
    out.commitBoundarySec = m_commitBoundarySec;

    asr::SessionState &state = out.state;
    state.title = m_title;
    state.confThresholdPct = kConfThresholdPct;
    const bool windowed = viewStartSec >= 0.0 || viewEndSec >= 0.0;
    for (const asr::DisplayRow &row : m_rows) {
        if (windowed) {
            // Any overlap with the window keeps the row: clipping a row in half
            // would hand the review pane a sentence with no beginning.
            if (viewStartSec >= 0.0 && row.endSec < viewStartSec)
                continue;
            if (viewEndSec >= 0.0 && row.startSec > viewEndSec)
                continue;
        }
        state.rows.append(row);
    }
    if (m_haveProvisional)
        state.provisionalRows.append(m_provisional);
    state.speakerIds = speakerIds();
    state.nPhrases = m_nPhrases;
    state.nLow = m_nLow;
    state.ampTrace = m_ampTrace;
    state.ampTraceStepSec = m_ampStepSec;
    state.sourceTotalSec = m_sourceSeenSec;
    state.sourceSeenSec = m_sourceSeenSec;
    state.speechSeenSec = m_speechSeenSec;
    state.wallElapsedSec = m_startedAt > 0.0 ? nowSeconds() - m_startedAt : 0.0;
    state.playheadRatio = m_sourceSeenSec > 0.0 ? 1.0 : 0.0;
    state.done = m_done;
    state.ts = nowSeconds();
    state.lastAsrChunkMs = quint32(m_lastChunkMs);
    return out;
}

asr::CanonicalTranscript LiveTranscript::transcript() const
{
    asr::CanonicalTranscript out;
    out.revision = m_revision;
    out.final = m_done;
    out.commitBoundarySec = m_commitBoundarySec;
    for (const asr::DisplayRow &row : m_rows) {
        for (const asr::Word &word : row.displayTokens) {
            out.words.append(word);
            if (!out.text.isEmpty())
                out.text += QLatin1Char(' ');
            out.text += word.w;
        }
    }
    return out;
}

bool LiveTranscript::applyEdit(quint64 baseRevision, double startSec, double endSec,
                               const QList<asr::Word> &words)
{
    if (baseRevision != m_revision)
        return false;

    // Rebuild from the flat word list: splicing rows in place would have to get
    // turn boundaries right a second time, and the two answers would drift.
    QList<asr::Word> kept;
    QString speakerAtRange;
    for (const asr::DisplayRow &row : m_rows) {
        for (const asr::Word &word : row.displayTokens) {
            const bool inside = word.startSec >= startSec && word.startSec < endSec;
            if (inside) {
                if (speakerAtRange.isEmpty())
                    speakerAtRange = word.speaker;
                continue;
            }
            kept.append(word);
        }
    }
    for (const asr::Word &incoming : words) {
        asr::Word word = incoming;
        if (word.speaker.isEmpty())
            word.speaker = speakerAtRange;
        // An editor that gives no timing gets the range it replaced, so the
        // word still sorts into the right place in the meeting.
        if (word.startSec <= 0.0)
            word.startSec = startSec;
        if (word.endSec <= word.startSec)
            word.endSec = endSec;
        kept.append(word);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const asr::Word &a, const asr::Word &b) {
        return a.startSec < b.startSec;
    });

    // Keep the verified names that were attached to each diarization slot; they
    // are a property of the speaker, not of the words being replaced.
    QHash<QString, QString> verifiedBySpeaker;
    for (const asr::DisplayRow &row : m_rows) {
        if (!row.verifiedName.isEmpty())
            verifiedBySpeaker.insert(row.speaker, row.verifiedName);
    }

    m_rows.clear();
    m_nextRowId = 1;
    for (const asr::Word &word : kept)
        appendWords({word}, word.speaker, 0.0f, verifiedBySpeaker.value(word.speaker));

    ++m_revision;
    ++m_version;
    recount();
    return true;
}

void LiveTranscript::renameSpeaker(const QString &from, const QString &to,
                                   const QString &verifiedName)
{
    for (asr::DisplayRow &row : m_rows) {
        if (row.speaker != from)
            continue;
        if (!to.isEmpty())
            row.speaker = to;
        // Always applied, including when empty: blank is a deliberate "clear
        // the name", and proto3 cannot tell that from "not set".
        row.verifiedName = verifiedName;
        for (asr::Word &word : row.displayTokens) {
            if (word.speaker == from && !to.isEmpty())
                word.speaker = to;
        }
        rebuildPhrases(&row);
    }
    if (m_haveProvisional && m_provisional.speaker == from) {
        if (!to.isEmpty())
            m_provisional.speaker = to;
        m_provisional.verifiedName = verifiedName;
    }
    ++m_revision;
    ++m_version;
}
