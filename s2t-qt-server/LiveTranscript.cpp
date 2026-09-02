#include "LiveTranscript.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// asr_diar_session answers -1 while diarization has not decided who is speaking
// yet, which is not the same as speaker 0.  Treated as "no slot", so those
// words wait for the rolling window to re-send them with a real one instead of
// freezing into a lane of their own - the client renders DisplayRow.speaker as
// a bare integer, so a "-1" lane surfaces as the phantom speaker "Người 0".
bool isUnassignedSlot(const QString &speaker)
{
    return speaker.isEmpty() || speaker == QLatin1String("-1");
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

void LiveTranscript::configure(const QString &title, quint32 sampleRate, quint32 channels,
                               double sourceTotalSec)
{
    m_title = title;
    m_sourceTotalSec = sourceTotalSec;
    m_sampleRate = qMax(1u, sampleRate);
    m_channels = qMax(1u, channels);
    m_startedAt = nowSeconds();
    // Diarization slots are per stream: carrying a previous meeting's turns
    // over would place this one's first words on last one's speakers.
    m_turns.clear();
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

    // Folded before any word is placed: a word is assigned the slot that was
    // speaking at ITS OWN timestamps, and the subframes covering those
    // timestamps have to be on the timeline before the lookup happens.
    foldDiarization(response.diarization);

    // asr_words is a ROLLING WINDOW, not a delta.
    //
    // asr_diar_session re-emits every word its ASR window still covers on each
    // chunk - about eight seconds of them - so appending what arrives produces
    // "học học học học học". The words carry stable timestamps, which is what
    // makes the fix exact: replace the span they cover rather than add to it.
    // A re-sent word lands on its own previous position and the transcript
    // stops growing sideways.
    if (!response.asrWords.isEmpty()) {
        replaceSpan(response.asrWords, response.speaker, response.speakerProb,
                    response.verifiedName);
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

    // The correction pass comes AFTER the new words, never before.
    //
    // It rewrites words the tier had already emitted, including the ones that
    // just arrived in this same chunk.  Applying it first and then appending
    // asr_words puts those words back a second time, which is exactly the
    // duplicated half-sentence this ordering exists to prevent.
    //
    // Its range is the range its own words cover - min start to max end - and
    // nothing wider.  Synthesising a wider one would tell us to wipe canonical
    // text that the correction never touched; grpc_session_adapter.py's
    // _correction_bounds carries the same rule and the same warning.
    // A correction rewrites TEXT, never who said it - so it carries no speaker
    // of its own, exactly like an operator edit, and every corrected word
    // inherits the slot and name of the word it replaces.
    //
    // Stamping the chunk's speaker across it instead is what wrecked the
    // transcript before 2026-09-02: merged_words routinely spans minutes of
    // meeting, so one correction relabelled everything it covered as whoever
    // happened to be talking at that instant.  Measured on the 5-minute sample:
    // the row at 00:21 was labelled Newsman, then Anna, then nobody, then
    // "Người 4" - same words, four different speakers, purely from corrections
    // rolling over it.
    if (!response.correction.mergedWords.isEmpty())
        replaceSpan(response.correction.mergedWords, QString(), 0.0f, QString());

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

void LiveTranscript::replaceSpan(const QList<asr::Word> &words, const QString &speaker,
                                 float speakerProb, const QString &verifiedName)
{
    // The span the incoming words cover, and nothing wider.  Widening it would
    // wipe canonical text these words never touched - the same rule
    // grpc_session_adapter.py's _correction_bounds carries, and the same
    // warning it carries about why.
    double from = 0.0;
    double to = 0.0;
    bool have = false;
    for (const asr::Word &word : words) {
        if (word.endSec <= word.startSec)
            continue; // no usable timing: it cannot define a span
        if (!have) {
            from = word.startSec;
            to = word.endSec;
            have = true;
        } else {
            from = qMin(from, word.startSec);
            to = qMax(to, word.endSec);
        }
    }
    if (!have) {
        // No timing anywhere in the batch: there is no span to replace, so the
        // only honest thing left is to append and let the turn rule sort it.
        appendWords(words, speaker, speakerProb, verifiedName);
        ++m_revision;
        return;
    }
    // The chunk's own speaker and verified name have to travel with its words.
    // They used to stop here - replaceSpan() called the operator-edit path,
    // which has no speaker arguments - and because asr_words always carries
    // timings, that was the path every single chunk took.  The result was a
    // transcript whose committed rows had an empty speaker and no name at all
    // while the provisional row, set directly in apply(), showed both.
    spliceWords(m_revision, from, to, words, speaker, speakerProb, verifiedName);
}

void LiveTranscript::appendWords(const QList<asr::Word> &words, const QString &speaker,
                                 float speakerProb, const QString &verifiedName)
{
    for (const asr::Word &incoming : words) {
        asr::Word word = incoming;
        // The tier may tag words individually or only per chunk; a per-word tag
        // wins, because diarization inside a chunk is the finer answer.
        if (isUnassignedSlot(word.speaker))
            word.speaker = isUnassignedSlot(speaker) ? QString() : speaker;

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
        // QChar chứ không phải QLatin1Char cho dấu ba chấm: QLatin1Char nhận một
        // char, nên U+2026 bị cắt xuống byte thấp và phép so sánh hoá ra là hỏi
        // "có phải dấu &".  Ba dấu kia là ASCII nên QLatin1Char vẫn đúng.
        const bool endsSentence = last == QLatin1Char('.') || last == QLatin1Char('?')
            || last == QLatin1Char('!') || last == QChar(u'…');
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
    // A file has a known length; a live meeting does not, so the best answer
    // there is how much has been seen.
    state.sourceTotalSec = m_sourceTotalSec > 0.0 ? m_sourceTotalSec : m_sourceSeenSec;
    state.sourceSeenSec = m_sourceSeenSec;
    state.speechSeenSec = m_speechSeenSec;
    state.wallElapsedSec = m_startedAt > 0.0 ? nowSeconds() - m_startedAt : 0.0;
    state.playheadRatio = state.sourceTotalSec > 0.0
        ? qBound(0.0, m_sourceSeenSec / state.sourceTotalSec, 1.0)
        : 0.0;
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

void LiveTranscript::foldDiarization(const asr::Diarization &diarization)
{
    // diar_chunk_preds_flat is a [subframes x speakers] score matrix, flattened
    // row-major, with one start/end pair per row.  The winning column is the
    // slot; below kDiarFloor nobody is speaking clearly enough to claim the
    // subframe, and saying nothing is better than inventing a turn out of room
    // tone.
    constexpr float kDiarFloor = 0.5f;

    if (diarization.shape.size() != 2)
        return;
    const int rows = diarization.shape.at(0);
    const int cols = diarization.shape.at(1);
    if (rows <= 0 || cols <= 0 || diarization.flatScores.size() < rows * cols)
        return;
    if (diarization.subframeStartMs.size() < rows || diarization.subframeEndMs.size() < rows)
        return;

    for (int row = 0; row < rows; ++row) {
        int best = -1;
        float bestScore = kDiarFloor;
        for (int col = 0; col < cols; ++col) {
            const float score = diarization.flatScores.at(row * cols + col);
            if (score > bestScore) {
                bestScore = score;
                best = col;
            }
        }
        if (best < 0)
            continue;

        const double from = double(diarization.subframeStartMs.at(row)) / 1000.0;
        const double to = double(diarization.subframeEndMs.at(row)) / 1000.0;
        if (to <= from)
            continue;
        // The tier re-sends the window it is working on, so a subframe already
        // folded in must not extend the timeline a second time.
        if (!m_turns.isEmpty() && to <= m_turns.last().endSec + 1e-6)
            continue;

        const QString slot = QString::number(best);
        if (!m_turns.isEmpty() && m_turns.last().speaker == slot
            && from <= m_turns.last().endSec + 0.2) {
            m_turns.last().endSec = to;
        } else {
            m_turns.append({from, to, slot});
        }
    }
}

QString LiveTranscript::speakerAt(double startSec, double endSec) const
{
    if (endSec <= startSec)
        endSec = startSec + 1e-3;
    QString best;
    double bestOverlap = 0.0;
    for (const DiarTurn &turn : m_turns) {
        if (turn.endSec <= startSec)
            continue;
        if (turn.startSec >= endSec)
            break; // kept in time order
        const double overlap = qMin(endSec, turn.endSec) - qMax(startSec, turn.startSec);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = turn.speaker;
        }
    }
    return best;
}

bool LiveTranscript::isPlaceholderName(const QString &name)
{
    const QString value = name.trimmed().toLower();
    if (value.isEmpty())
        return true;
    // Kept in step with TranscriptModel::isRealName() in the client.  Both
    // lists have to agree: this one decides what is stored, that one decides
    // what is drawn, and a name that passes here only to be hidden there is a
    // row that looks unidentified for no reason anybody can see.
    static const QSet<QString> placeholders = {
        QStringLiteral("unknown"), QStringLiteral("unk"), QStringLiteral("?"),
        QStringLiteral("spk?"), QStringLiteral("speaker?"),
    };
    if (placeholders.contains(value))
        return true;
    if (value.startsWith(QStringLiteral("unknown_")) || value.startsWith(QStringLiteral("unknown-")))
        return true;
    static const QRegularExpression bare(QStringLiteral("^speaker_\\d+$"));
    return bare.match(value).hasMatch();
}

bool LiveTranscript::applyEdit(quint64 baseRevision, double startSec, double endSec,
                               const QList<asr::Word> &words)
{
    // An operator edit changes text, never who said it, so it passes no
    // speaker of its own and every word keeps the slot it already had.
    return spliceWords(baseRevision, startSec, endSec, words, QString(), 0.0f, QString());
}

bool LiveTranscript::spliceWords(quint64 baseRevision, double startSec, double endSec,
                                 const QList<asr::Word> &words, const QString &speaker,
                                 float speakerProb, const QString &verifiedName)
{
    if (baseRevision != m_revision)
        return false;

    const QString chunkSpeaker = isUnassignedSlot(speaker) ? QString() : speaker;

    // The name a word was given when it arrived travels with the word, not with
    // its diarization slot.  Keying it on the slot instead means every later
    // answer for that slot rewrites history: the tier flips between the
    // candidates it is weighing, and a meeting where Newsman spoke first ends
    // up with all of his lines relabelled as whoever was named last.
    // grpc_session_adapter.py carries the same rule as `_verified_name` on each
    // word rather than on the row.
    struct Placed
    {
        asr::Word word;
        QString name;
        float prob = 0.0f;
    };

    // Rebuild from the flat word list: splicing rows in place would have to get
    // turn boundaries right a second time, and the two answers would drift.
    QList<Placed> kept;
    QList<Placed> replaced; // what used to be in the span, in time order
    QHash<QString, QString> nameBySpeaker;
    for (const asr::DisplayRow &row : m_rows) {
        if (!row.verifiedName.isEmpty())
            nameBySpeaker.insert(row.speaker, row.verifiedName);
        for (const asr::Word &word : row.displayTokens) {
            const bool inside = word.startSec >= startSec && word.startSec < endSec;
            if (inside)
                replaced.append({word, row.verifiedName, row.speakerProb});
            else
                kept.append({word, row.verifiedName, row.speakerProb});
        }
    }

    // Who was speaking at `at`, according to the words being replaced.  Per
    // word rather than one answer for the whole span: a correction can cover
    // several turns, and giving all of them the first speaker's slot merges
    // people who were told apart correctly the first time.
    const auto priorAt = [&replaced](double at) -> const Placed * {
        const Placed *best = nullptr;
        for (const Placed &item : replaced) {
            if (item.word.startSec <= at + 1e-6)
                best = &item;
            if (at >= item.word.startSec && at < item.word.endSec)
                return &item;
        }
        return best;
    };

    // The word that used to sit at exactly this position, if there was one.
    // Strictly containment, unlike priorAt(): "which word is this one
    // replacing" and "who was talking around here" are different questions,
    // and answering the first with the nearest earlier word would freeze a
    // brand-new word onto the previous speaker's lane.
    const auto placedAt = [&replaced](double at) -> const Placed * {
        for (const Placed &item : replaced) {
            if (at >= item.word.startSec - 1e-6 && at < item.word.endSec)
                return &item;
        }
        return nullptr;
    };
    for (const asr::Word &incoming : words) {
        asr::Word word = incoming;
        // An editor that gives no timing gets the range it replaced, so the
        // word still sorts into the right place in the meeting.
        if (word.startSec <= 0.0)
            word.startSec = startSec;
        if (word.endSec <= word.startSec)
            word.endSec = endSec;

        const Placed *prior = priorAt(word.startSec);
        const Placed *already = placedAt(word.startSec);

        // A word's lane is frozen the first time it is placed.
        //
        // asr_words is a rolling window about eight seconds wide and the chunk
        // carries ONE speaker for all of it, so stamping that speaker across
        // the batch relabels every turn boundary inside the window to whoever
        // is talking right now.  In an interview - where the turns are a few
        // seconds apart - that swaps the question onto the person answering
        // it.  Measured on the 2:33 sample: "điểm mạnh của bạn là gì" ended up
        // under the candidate and her answer under the interviewer.
        //
        // So the chunk's speaker applies only to words this window has not
        // placed before.  s2t-qt-client/core/TranscriptModel.cpp freezes lanes
        // the same way, for the same reason, and says so.
        if (isUnassignedSlot(word.speaker)) {
            // A word that has been placed keeps its lane - the freeze has to
            // win, or a correction that nudges the timings by a few
            // milliseconds re-runs the lookup and can land the word on the
            // other side of a turn boundary.  For a word being placed for the
            // FIRST time, diarization at that word's own timestamps is the
            // right source: it is the only one that knows who was speaking
            // then rather than now.
            if (already && !isUnassignedSlot(already->word.speaker))
                word.speaker = already->word.speaker;
            else if (const QString atWord = speakerAt(word.startSec, word.endSec);
                     !atWord.isEmpty())
                word.speaker = atWord;
            else if (!chunkSpeaker.isEmpty())
                word.speaker = chunkSpeaker;
            else if (prior)
                word.speaker = prior->word.speaker;
        }

        // The name for the words that just arrived.  A placeholder does not
        // erase what the word is already called: the tier goes on emitting
        // "unknown" for chunks it cannot score, and letting those through
        // would make an identified speaker lose their name every few hundred
        // milliseconds.
        QString name;
        float prob = speakerProb;
        if (already && !isPlaceholderName(already->name)) {
            // Already identified: keep it.  Only one upgrade is allowed per
            // word, from "no name" to a verified one - the same single-step
            // rule TranscriptModel applies to lanes, and what stops a word
            // changing hands every time the window rolls over it.
            name = already->name;
            prob = already->prob;
        } else if (!isPlaceholderName(verifiedName) && word.speaker == chunkSpeaker) {
            // The chunk's name belongs to the chunk's slot.  Now that a word can
            // land on a different slot than the one being decoded, handing it
            // this name anyway would put the person who is talking now onto the
            // words of the person who was talking then.
            name = verifiedName.trimmed();
        } else if (prior) {
            name = prior->name;
            prob = prior->prob;
        } else {
            name = nameBySpeaker.value(word.speaker);
        }
        kept.append({word, name, prob});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Placed &a, const Placed &b) {
        return a.word.startSec < b.word.startSec;
    });

    m_rows.clear();
    m_nextRowId = 1;
    for (const Placed &placed : kept)
        appendWords({placed.word}, placed.word.speaker, placed.prob, placed.name);

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
