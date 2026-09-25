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

// Shortest stretch worth staging as evidence for a voice.  Below this it is an
// interjection - "vâng", "dạ" - and CAM++ gets a worse embedding from a pile of
// those than from one real sentence.
const double kMinEvidenceSpanSec = 1.0;

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

// Does this token close a sentence?
//
// The mark is not always the last character: a quoted or bracketed sentence
// ends `rồi."` or `rồi.)`, so the closers are skipped before the mark is read.
// QChar and not QLatin1Char for the ellipsis: QLatin1Char takes one char, so
// U+2026 gets cut down to its low byte and the comparison silently becomes
// "is this an ampersand".  The other three are ASCII and are safe either way.
bool closesSentence(const QString &token)
{
    int i = token.size() - 1;
    while (i >= 0) {
        const QChar ch = token.at(i);
        if (ch == QLatin1Char(']') || ch == QLatin1Char(')') || ch == QLatin1Char('}')
            || ch == QLatin1Char('"') || ch == QLatin1Char('\'') || ch == QChar(u'”')
            || ch == QChar(u'’')) {
            --i;
            continue;
        }
        break;
    }
    if (i < 0)
        return false;
    const QChar last = token.at(i);
    return last == QLatin1Char('.') || last == QLatin1Char('?') || last == QLatin1Char('!')
        || last == QChar(u'…');
}

// Upper-cases the first LETTER of a token, not its first character: a token can
// open with a bracket or a quote, and `("hôm` must become `("Hôm`.
//
// Nothing is ever lowered.  A capital in the middle of a sentence is the
// tier's CASE label deciding this is a proper noun, and flattening it would
// undo a decision a model made on purpose.
QString sentenceCased(const QString &token)
{
    for (int i = 0; i < token.size(); ++i) {
        if (!token.at(i).isLetter())
            continue;
        QString out = token;
        out[i] = token.at(i).toUpper();
        return out;
    }
    return token;
}

// Two surfaces of the same word, compared without the mark or the capital:
// the correction pass and the streaming pass spell a word differently
// ("kiểm" against "kiểm,") and that difference must not make them look like
// two different words.
QString wordCore(const QString &token)
{
    QString out;
    out.reserve(token.size());
    for (const QChar &ch : token) {
        if (ch.isLetterOrNumber())
            out.append(ch.toLower());
    }
    return out;
}

bool hasLetter(const QString &token)
{
    for (const QChar &ch : token) {
        if (ch.isLetter())
            return true;
    }
    return false;
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
    m_pending.clear();
    m_pendingRows.clear();
    m_manualSpans.clear();
    m_nameBySlot.clear();
}

// Everything the tier reports, moved onto the meeting's own timeline.
//
// A resumed meeting opens a new stream and that stream counts from zero, so
// without this the words of the second half sit on top of the first and a
// reviewer clicking a sentence at 30 s hears the audio from 0.4 s.  Every
// field carrying a time has to move together - words, the correction, the
// chunk bounds, the commit boundary, the diarization subframes and the two
// "seen" counters - or the parts of one answer stop agreeing with each other.
asr::PushAudioResponse LiveTranscript::shifted(const asr::PushAudioResponse &response) const
{
    if (qFuzzyIsNull(m_timeOffsetSec))
        return response;
    const double offset = m_timeOffsetSec;
    asr::PushAudioResponse out = response;
    const auto move = [offset](QList<asr::Word> *words) {
        for (asr::Word &word : *words) {
            word.startSec += offset;
            word.endSec += offset;
        }
    };
    move(&out.asrWords);
    move(&out.correction.mergedWords);
    if (out.chunkStartSec > 0.0 || out.chunkEndSec > 0.0) {
        out.chunkStartSec += offset;
        out.chunkEndSec += offset;
    }
    if (out.correction.commitBoundarySec > 0.0)
        out.correction.commitBoundarySec += offset;
    if (out.sourceSeenSec > 0.0)
        out.sourceSeenSec += offset;
    if (out.speechSeenSec > 0.0)
        out.speechSeenSec += offset;
    const qint64 offsetMs = qint64(offset * 1000.0);
    for (qint64 &value : out.diarization.subframeStartMs)
        value += offsetMs;
    for (qint64 &value : out.diarization.subframeEndMs)
        value += offsetMs;
    return out;
}

quint64 LiveTranscript::apply(const asr::PushAudioResponse &incoming)
{
    const asr::PushAudioResponse response = shifted(incoming);
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

    // Who the tier says this slot is.  Only the streaming chunk carries a
    // verified name; the correction that actually puts the word into the
    // official transcript carries none, so without remembering it here a
    // meeting with a matched CAM++ identity shows every row unnamed.
    if (!isUnassignedSlot(response.speaker) && !isPlaceholderName(response.verifiedName))
        m_nameBySlot.insert(response.speaker, response.verifiedName.trimmed());

    // asr_words is a ROLLING WINDOW, not a delta, and it is a GUESS.
    //
    // asr_diar_session re-emits every word its ASR window still covers on each
    // chunk - about eight seconds of them - so appending what arrives produces
    // "học học học học học". The words carry stable timestamps, which is what
    // makes the fix exact: replace the span they cover rather than add to it.
    //
    // They land in the provisional lane, never in the official transcript.
    // Everything past commit_boundary_sec is still being re-decided by the
    // correction pass, and a word that reaches the official lane early is
    // written twice when the correction finally covers it - the "Hôm Hôm nay"
    // in the acceptance report.  m_pending is that lane; the correction below
    // is the only thing that promotes a word out of it.
    if (!response.asrWords.isEmpty()) {
        mergePending(response.asrWords, response.speaker, response.speakerProb,
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
    if (!response.correction.mergedWords.isEmpty()) {
        replaceSpan(response.correction.mergedWords, QString(), 0.0f, QString());
        // The correction owns these seconds now.  Whatever the edge was
        // holding for them is not a second opinion, it is the older guess.
        double from = 0.0;
        double to = 0.0;
        bool have = false;
        for (const asr::Word &word : response.correction.mergedWords) {
            if (word.endSec <= word.startSec)
                continue;
            from = have ? qMin(from, word.startSec) : word.startSec;
            to = have ? qMax(to, word.endSec) : word.endSec;
            have = true;
        }
        if (have)
            dropPendingInSpan(from, to);
    }
    // Anything the tier has committed is the official transcript's business,
    // even when this particular correction did not mention it.
    if (m_commitBoundarySec > 0.0)
        dropPendingInSpan(0.0, m_commitBoundarySec);
    // ...and the reverse: a correction window routinely reaches a little past
    // the boundary it reports, and those last few words are still going to be
    // re-decided.  They go back to the edge until the boundary catches up, so
    // the official transcript never contains a word the tier has not committed.
    holdBackUncommitted();
    rebuildProvisional();

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

bool LiveTranscript::overlapsManualEdit(double startSec, double endSec) const
{
    for (const QPair<double, double> &span : m_manualSpans) {
        if (qMin(endSec, span.second) - qMax(startSec, span.first) > 0.0)
            return true;
    }
    return false;
}

void LiveTranscript::mergePending(const QList<asr::Word> &words, const QString &speaker,
                                  float speakerProb, const QString &verifiedName)
{
    double from = 0.0;
    double to = 0.0;
    bool have = false;
    for (const asr::Word &word : words) {
        if (word.endSec <= word.startSec)
            continue;
        from = have ? qMin(from, word.startSec) : word.startSec;
        to = have ? qMax(to, word.endSec) : word.endSec;
        have = true;
    }
    if (!have)
        return;

    // The window this batch covers is replaced, not added to - a re-sent word
    // lands on its own previous position instead of beside it.
    QList<asr::Word> kept;
    kept.reserve(m_pending.size() + words.size());
    for (const asr::Word &word : m_pending) {
        if (word.startSec >= from - 1e-6 && word.startSec < to)
            continue;
        kept.append(word);
    }
    for (const asr::Word &incoming : words) {
        if (incoming.w.trimmed().isEmpty())
            continue;
        // A word the tier has already committed belongs to the official lane,
        // and a span somebody edited by hand belongs to them.
        if (incoming.endSec <= m_commitBoundarySec + 1e-3)
            continue;
        if (overlapsManualEdit(incoming.startSec, incoming.endSec))
            continue;
        asr::Word word = incoming;
        if (isUnassignedSlot(word.speaker)) {
            const QString atWord = speakerAt(word.startSec, word.endSec);
            word.speaker = !atWord.isEmpty() ? atWord
                                             : (isUnassignedSlot(speaker) ? QString() : speaker);
        }
        kept.append(word);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const asr::Word &a, const asr::Word &b) {
        return a.startSec < b.startSec;
    });
    m_pending = kept;
    if (!isPlaceholderName(verifiedName))
        m_pendingName = verifiedName.trimmed();
    if (speakerProb > 0.0f)
        m_pendingProb = speakerProb;
}

void LiveTranscript::holdBackUncommitted()
{
    if (m_done || m_commitBoundarySec <= 0.0)
        return;
    QList<asr::Word> released;
    bool changed = false;
    QList<asr::Word> keptWords;
    QHash<QString, QString> nameBySpeaker;
    for (const asr::DisplayRow &row : m_rows) {
        if (!row.verifiedName.isEmpty())
            nameBySpeaker.insert(row.speaker, row.verifiedName);
        for (const asr::Word &word : row.displayTokens) {
            if (word.endSec > m_commitBoundarySec + 1e-3
                && !overlapsManualEdit(word.startSec, word.endSec)) {
                released.append(word);
                changed = true;
                continue;
            }
            keptWords.append(word);
        }
    }
    if (!changed)
        return;

    m_rows.clear();
    m_nextRowId = 1;
    for (const asr::Word &word : keptWords)
        appendWords({word}, word.speaker, m_pendingProb, nameBySpeaker.value(word.speaker));

    // Back onto the edge, where a later correction will find them again.
    for (const asr::Word &word : released) {
        bool already = false;
        for (const asr::Word &pending : m_pending) {
            if (qMin(pending.endSec, word.endSec) - qMax(pending.startSec, word.startSec) > 0.0) {
                already = true;
                break;
            }
        }
        if (!already)
            m_pending.append(word);
    }
    std::stable_sort(m_pending.begin(), m_pending.end(),
                     [](const asr::Word &a, const asr::Word &b) {
                         return a.startSec < b.startSec;
                     });
}

void LiveTranscript::dropPendingInSpan(double startSec, double endSec)
{
    QList<asr::Word> kept;
    kept.reserve(m_pending.size());
    for (const asr::Word &word : m_pending) {
        // Strictly by the word's own start, the same rule spliceWords() uses,
        // so the two lanes cannot disagree about which side a word is on.
        if (word.startSec >= startSec - 1e-6 && word.startSec < endSec + 1e-3)
            continue;
        kept.append(word);
    }
    m_pending = kept;
}

void LiveTranscript::rebuildProvisional()
{
    m_pendingRows.clear();
    int index = 0;
    for (const asr::Word &word : m_pending) {
        const bool sameTurn = !m_pendingRows.isEmpty()
            && m_pendingRows.last().speaker == word.speaker
            && (word.startSec - m_pendingRows.last().endSec) <= kTurnGapSec;
        if (!sameTurn) {
            asr::DisplayRow row;
            // Ids of their own, stable across polls while the row lasts: the
            // client keys its lane locks on them and would otherwise treat one
            // moving row as a new one every 160 ms.
            row.rowId = QStringLiteral("prov%1").arg(++index);
            row.speaker = word.speaker;
            row.speakerProb = m_pendingProb;
            row.verifiedName = m_pendingName;
            row.startSec = word.startSec;
            row.endSec = word.endSec;
            row.isProvisional = true;
            m_pendingRows.append(row);
        }
        asr::DisplayRow &row = m_pendingRows.last();
        row.displayTokens.append(word);
        row.endSec = qMax(row.endSec, word.endSec);
    }
    for (asr::DisplayRow &row : m_pendingRows) {
        if (row.verifiedName.isEmpty())
            row.verifiedName = m_nameBySlot.value(row.speaker);
        // Never the start of a sentence: the edge is the middle of whatever
        // the official transcript was saying.
        rebuildPhrases(&row, false);
        row.isProvisional = true;
        row.updatingText = row.mergedText;
        row.updatingTokens = row.displayTokens;
    }
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
    spliceWords(m_revision, from, to, words, speaker, speakerProb, verifiedName, true);
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
        rebuildPhrasesAt(m_rows.size() - 1);
}

bool LiveTranscript::rowOpensSentence(int index) const
{
    if (index <= 0)
        return true; // the meeting opens one
    const asr::DisplayRow &previous = m_rows.at(index - 1);
    if (previous.displayTokens.isEmpty())
        return true;
    // A row break is a turn or a pause, not a full stop.  Whether a capital
    // belongs here is decided by the punctuation of the word before it, which
    // is the same rule _sentence_case_itn_parts uses over the rendered text.
    return closesSentence(previous.displayTokens.last().w);
}

void LiveTranscript::rebuildPhrasesAt(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    rebuildPhrases(&m_rows[index], rowOpensSentence(index));
}

void LiveTranscript::rebuildPhrases(asr::DisplayRow *row, bool opensSentence) const
{
    row->phrases.clear();
    row->mergedText.clear();
    row->stableTokenCount = quint32(row->displayTokens.size());

    // One phrase per sentence, split on end punctuation.  The tier's
    // punctuation model is what makes this meaningful, and since 2026-09-21 it
    // actually reaches here: the words carry itn_part_text, mark included, so
    // a row is now as many phrases as it has sentences instead of always one.
    //
    // Sentence case is applied in the same pass, and it has to be: the tier
    // does NOT capitalise its word records.  _sentence_case_itn_parts
    // (itn_merge.py:313) runs over the whole rendered text instead, so the
    // first word after a full stop arrives as "tăng" and would stay that way
    // if every layer only looked at one word at a time.
    //
    // It is applied to the COPY that goes into the phrase, never to
    // displayTokens.  Writing the capital back into the token makes it
    // permanent: a later correction that moves the sentence boundary cannot
    // take it off again, so a word ends up capitalised in the middle of a
    // sentence with nothing left to explain why.  The client renders the
    // phrase surface (TranscriptModel::relabelFromSurface prefers it), so the
    // capital still reaches the screen.
    asr::Phrase phrase;
    float sum = 0.0f;
    int count = 0;
    bool sentenceStart = opensSentence;
    for (const asr::Word &token : row->displayTokens) {
        asr::Word word = token;
        if (sentenceStart)
            word.w = sentenceCased(word.w);
        // A token with no letter in it - a stray mark - neither takes the
        // capital nor spends it.
        if (hasLetter(word.w))
            sentenceStart = false;
        if (phrase.words.isEmpty())
            phrase.startSec = word.startSec;
        phrase.words.append(word);
        phrase.endSec = word.endSec;
        sum += word.c;
        ++count;
        if (!phrase.text.isEmpty())
            phrase.text += QLatin1Char(' ');
        phrase.text += word.w;

        const bool endsSentence = closesSentence(word.w);
        if (endsSentence) {
            sentenceStart = true;
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

QList<QPair<double, double>> LiveTranscript::speakerSpans(const QString &speaker,
                                                          double maxSec) const
{
    QList<QPair<double, double>> runs;
    if (speaker.isEmpty())
        return runs;

    // Words, not rows.  A row is a turn as the display draws it and can hold a
    // word the diarization gave to somebody else; the word carries the slot it
    // was actually placed in.
    //
    // Every word is walked, not only this speaker's, and in time order -
    // because a run has to BREAK where somebody else spoke.  Joining this
    // speaker's words across a gap that another voice sits in would hand CAM++
    // a span with two people in it, and a blended embedding is the worst
    // outcome available here: it does not fail, it quietly mis-names people in
    // every later meeting.
    struct Placed
    {
        double startSec;
        double endSec;
        QString slot;
    };
    QList<Placed> words;
    for (const asr::DisplayRow &row : m_rows) {
        for (const asr::Word &word : row.displayTokens) {
            if (word.endSec <= word.startSec)
                continue;
            words.append({word.startSec, word.endSec,
                          word.speaker.isEmpty() ? row.speaker : word.speaker});
        }
    }
    std::sort(words.begin(), words.end(),
              [](const Placed &a, const Placed &b) { return a.startSec < b.startSec; });

    bool open = false;
    for (const Placed &word : words) {
        if (word.slot != speaker) {
            open = false; // somebody else: whatever run was building ends here
            continue;
        }
        if (open && word.startSec - runs.last().second <= kTurnGapSec) {
            runs.last().second = qMax(runs.last().second, word.endSec);
            continue;
        }
        runs.append({word.startSec, word.endSec});
        open = true;
    }

    // Longest first, so a cap keeps the cleanest evidence rather than whatever
    // happened to be said first.  A one-word "vâng" is worth nothing to CAM++
    // and would crowd out a real sentence.
    std::sort(runs.begin(), runs.end(),
              [](const QPair<double, double> &a, const QPair<double, double> &b) {
                  return (a.second - a.first) > (b.second - b.first);
              });

    QList<QPair<double, double>> out;
    double collected = 0.0;
    for (const QPair<double, double> &run : runs) {
        const double length = run.second - run.first;
        if (length < kMinEvidenceSpanSec)
            continue;
        out.append(run);
        collected += length;
        if (maxSec > 0.0 && collected >= maxSec)
            break;
    }
    // Back into time order: the publish path concatenates the PCM behind these
    // and hands CAM++ one sample, which should read as the meeting did.
    std::sort(out.begin(), out.end(),
              [](const QPair<double, double> &a, const QPair<double, double> &b) {
                  return a.first < b.first;
              });
    return out;
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

void LiveTranscript::noteProgress(double sourceSeenSec, double speechSeenSec)
{
    if (sourceSeenSec > m_sourceSeenSec)
        m_sourceSeenSec = sourceSeenSec;
    if (speechSeenSec > m_speechSeenSec)
        m_speechSeenSec = speechSeenSec;
    ++m_version;
}

void LiveTranscript::markDone()
{
    // The tail the correction pass never reached.  The final flush normally
    // covers everything, so this is usually empty - but when it is not, these
    // are real words that were spoken, and dropping them would silently
    // truncate the end of the meeting.
    //
    // A word is only promoted when the official lane has nothing over the same
    // seconds: the correction and the edge describe the same audio, and
    // promoting a word the correction already rewrote is exactly the duplicate
    // this lane exists to prevent.
    QList<asr::Word> promoted;
    for (const asr::Word &word : m_pending) {
        if (word.endSec <= m_commitBoundarySec + 1e-3)
            continue;
        if (overlapsManualEdit(word.startSec, word.endSec))
            continue;
        bool covered = false;
        for (const asr::DisplayRow &row : m_rows) {
            if (row.endSec < word.startSec - 0.3 || row.startSec > word.endSec + 0.3)
                continue;
            for (const asr::Word &placed : row.displayTokens) {
                const double overlap =
                    qMin(placed.endSec, word.endSec) - qMax(placed.startSec, word.startSec);
                if (overlap > 0.2 * qMax(0.01, word.endSec - word.startSec)) {
                    covered = true;
                    break;
                }
                // The same word, a fraction of a second away.  A correction
                // does not re-use the streaming pass's timings exactly, so the
                // settled copy of a word and the edge's copy of it routinely
                // sit next to each other rather than on top of each other -
                // and promoting the second one is what puts "kiểm kiểm" at the
                // end of a meeting.
                if (qAbs(placed.startSec - word.startSec) < 0.3
                    && wordCore(placed.w) == wordCore(word.w)) {
                    covered = true;
                    break;
                }
            }
            if (covered)
                break;
        }
        if (!covered)
            promoted.append(word);
    }
    if (!promoted.isEmpty()) {
        QList<asr::Word> flat;
        for (const asr::DisplayRow &row : m_rows)
            flat.append(row.displayTokens);
        flat.append(promoted);
        std::stable_sort(flat.begin(), flat.end(), [](const asr::Word &a, const asr::Word &b) {
            return a.startSec < b.startSec;
        });
        QHash<QString, QString> nameBySpeaker;
        for (const asr::DisplayRow &row : m_rows) {
            if (!row.verifiedName.isEmpty())
                nameBySpeaker.insert(row.speaker, row.verifiedName);
        }
        m_rows.clear();
        m_nextRowId = 1;
        for (const asr::Word &word : flat)
            appendWords({word}, word.speaker, m_pendingProb, nameBySpeaker.value(word.speaker));
        ++m_revision;
    }
    m_pending.clear();
    m_pendingRows.clear();

    if (m_haveProvisional && !m_provisional.displayTokens.isEmpty()) {
        // Whatever the edge held is now as settled as it will ever be.
        m_provisional.isProvisional = false;
        m_rows.append(m_provisional);
        rebuildPhrasesAt(m_rows.size() - 1);
    }
    m_haveProvisional = false;
    m_provisional = asr::DisplayRow();
    m_done = true;
    ++m_version;
    recount();
}

bool LiveTranscript::isEditable(double startSec, double endSec) const
{
    Q_UNUSED(startSec);
    // A finished meeting has no moving edge: every word in it is final, so
    // every word in it can be corrected.
    if (m_done)
        return true;
    return endSec <= m_commitBoundarySec + 1e-3;
}

void LiveTranscript::restore(const asr::StateResponse &state)
{
    m_rows = state.state.rows;
    m_pending.clear();
    m_pendingRows.clear();
    m_provisional = asr::DisplayRow();
    m_haveProvisional = false;
    m_revision = state.transcriptRevision;
    m_version = qMax(m_version, state.stateVersion);
    m_commitBoundarySec = state.commitBoundarySec;
    m_done = state.transcriptFinal;
    m_sourceSeenSec = qMax(m_sourceSeenSec, state.state.sourceSeenSec);
    m_speechSeenSec = qMax(m_speechSeenSec, state.state.speechSeenSec);
    if (state.state.sourceTotalSec > 0.0)
        m_sourceTotalSec = state.state.sourceTotalSec;
    if (!state.state.title.isEmpty())
        m_title = state.state.title;
    m_ampTrace = state.state.ampTrace;
    m_ampStepSec = state.state.ampTraceStepSec;
    // Row ids have to keep rising past whatever the snapshot already used, or
    // the next row collides with one the client has on screen.
    m_nextRowId = 1;
    for (const asr::DisplayRow &row : m_rows) {
        const QString id = row.rowId;
        if (!id.startsWith(QLatin1Char('r')))
            continue;
        bool ok = false;
        const int number = id.mid(1).toInt(&ok);
        if (ok && number >= m_nextRowId)
            m_nextRowId = number + 1;
    }
    // Names first: a resumed meeting has to keep calling a voice what it was
    // already calling it, and the tier's next chunk may take a while to say
    // so again.
    m_nameBySlot.clear();
    for (const asr::DisplayRow &row : m_rows) {
        if (!row.speaker.isEmpty() && !isPlaceholderName(row.verifiedName))
            m_nameBySlot.insert(row.speaker, row.verifiedName);
    }

    // The diarization timeline is not in the snapshot; it is rebuilt from the
    // words that were placed with it, so a resumed meeting still knows who
    // owned the seconds it has already transcribed.
    m_turns.clear();
    for (const asr::DisplayRow &row : m_rows) {
        for (const asr::Word &word : row.displayTokens) {
            if (word.endSec <= word.startSec || word.speaker.isEmpty())
                continue;
            if (!m_turns.isEmpty() && m_turns.last().speaker == word.speaker
                && word.startSec <= m_turns.last().endSec + kTurnGapSec) {
                m_turns.last().endSec = qMax(m_turns.last().endSec, word.endSec);
                continue;
            }
            m_turns.append({word.startSec, word.endSec, word.speaker});
        }
    }
    std::stable_sort(m_turns.begin(), m_turns.end(),
                     [](const DiarTurn &a, const DiarTurn &b) { return a.startSec < b.startSec; });

    // The moving edge of a snapshot becomes settled text.
    //
    // The words the tier had emitted but not yet corrected sit in the
    // provisional lane, and after a restart nothing will ever correct them:
    // the audio behind them was already consumed by the stream that died, so
    // it is not in the backlog and is never decoded again.  Leaving them in
    // the edge means the next commit boundary - which lands past them, on the
    // new stream's timeline - drops them silently.  Measured 2026-09-24: a
    // 45 s meeting killed at 30 s came back with 8 of 29 words in the 20-30 s
    // bucket, and that hole is exactly this.
    //
    // So they are promoted, and the commit boundary is moved to cover them.
    // That is the honest reading: they are as settled as they will ever be.
    double promotedTo = m_commitBoundarySec;
    for (const asr::DisplayRow &row : state.state.provisionalRows) {
        for (const asr::Word &word : row.displayTokens) {
            if (word.w.trimmed().isEmpty() || word.endSec <= word.startSec)
                continue;
            appendWords({word}, word.speaker.isEmpty() ? row.speaker : word.speaker,
                        row.speakerProb, row.verifiedName);
            promotedTo = qMax(promotedTo, word.endSec);
        }
    }
    m_commitBoundarySec = promotedTo;
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
        if (!windowed) {
            state.rows.append(row);
            continue;
        }
        if (viewStartSec >= 0.0 && row.endSec < viewStartSec)
            continue;
        if (viewEndSec >= 0.0 && row.startSec > viewEndSec)
            continue;
        // The row overlaps the window, but its words may run far outside it -
        // a turn is minutes long and the window is ten seconds.  Asking for
        // [20, 30] and getting the whole meeting back, which is what this did
        // until 2026-09-24, makes the review window useless for the thing it
        // exists for: playing one sentence against its own audio.
        asr::DisplayRow clipped = row;
        clipped.displayTokens.clear();
        for (const asr::Word &word : row.displayTokens) {
            if (viewStartSec >= 0.0 && word.endSec < viewStartSec)
                continue;
            if (viewEndSec >= 0.0 && word.startSec > viewEndSec)
                continue;
            clipped.displayTokens.append(word);
        }
        if (clipped.displayTokens.isEmpty())
            continue;
        clipped.startSec = clipped.displayTokens.first().startSec;
        clipped.endSec = clipped.displayTokens.last().endSec;
        // The row no longer opens where it did, so the capital it was given
        // does not belong to it any more either.
        rebuildPhrases(&clipped, false);
        state.rows.append(clipped);
    }
    // The moving edge is live-only: a windowed read is a review of settled
    // text, and a guess about the last second of a meeting has no place in it.
    if (!windowed) {
        state.provisionalRows.append(m_pendingRows);
        if (m_haveProvisional)
            state.provisionalRows.append(m_provisional);
    }
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
    if (!spliceWords(baseRevision, startSec, endSec, words, QString(), 0.0f, QString()))
        return false;

    // From here the tier may not write over these seconds.  A confirmed human
    // decision outranks any later machine correction - realtime_ui.py carries
    // the same rule as the `manual` flag on each word, and without it the
    // correction window that rolls over this span a few seconds later quietly
    // puts the old text back while the audit log still says the edit applied.
    double from = startSec;
    double to = endSec;
    for (const asr::Word &word : words) {
        if (word.endSec <= word.startSec)
            continue;
        from = qMin(from, word.startSec);
        to = qMax(to, word.endSec);
    }
    QList<QPair<double, double>> merged;
    merged.append({from, to});
    for (const QPair<double, double> &span : m_manualSpans) {
        if (qMin(merged.last().second, span.second) - qMax(merged.last().first, span.first)
            >= 0.0) {
            merged.last().first = qMin(merged.last().first, span.first);
            merged.last().second = qMax(merged.last().second, span.second);
        } else {
            merged.append(span);
        }
    }
    m_manualSpans = merged;
    dropPendingInSpan(from, to);
    rebuildProvisional();
    return true;
}

bool LiveTranscript::spliceWords(quint64 baseRevision, double startSec, double endSec,
                                 const QList<asr::Word> &words, const QString &speaker,
                                 float speakerProb, const QString &verifiedName,
                                 bool respectManual)
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
            // A word somebody typed is not the tier's to take back.  It stays
            // where it is and is not even offered as "what used to be here",
            // so the incoming batch cannot inherit its lane either.
            if (inside && respectManual && overlapsManualEdit(word.startSec, word.endSec)) {
                kept.append({word, row.verifiedName, row.speakerProb});
                continue;
            }
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
        if (respectManual && overlapsManualEdit(incoming.startSec, incoming.endSec))
            continue; // these seconds belong to whoever edited them
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
        // Last resort, and the one that makes speaker verification visible at
        // all: the name the tier gave this slot on the chunk that decoded it.
        // A correction carries no name, and since the correction is what puts
        // a word into the official transcript, every first placement would
        // otherwise be anonymous.  Only when nothing else supplied a name -
        // the freeze rule above still wins.
        if (isPlaceholderName(name))
            name = m_nameBySlot.value(word.speaker);
        kept.append({word, name, prob});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Placed &a, const Placed &b) {
        return a.word.startSec < b.word.startSec;
    });

    // Collapse a word against its own echo.
    //
    // Two correction windows can both report the same word with timings a
    // fraction of a second apart, and the span rule cannot see that: the
    // second batch only replaces the span ITS OWN words cover, so a copy
    // sitting just before that span survives.  What reaches the screen is
    // "Hôm Hôm nay" - measured on every session of the 2026-09-24 run.  The
    // reference adapter collapses the same way
    // (`_collapse_overlapping_repeats`), and for the same reason.
    //
    // The later copy wins: it is the one decided with context on both sides.
    // A genuine repetition - somebody saying "rất rất" - is several hundred
    // milliseconds apart and survives this.
    QList<Placed> collapsed;
    collapsed.reserve(kept.size());
    for (const Placed &item : kept) {
        if (!collapsed.isEmpty()) {
            Placed &previous = collapsed.last();
            const QString core = wordCore(item.word.w);
            if (!core.isEmpty() && core == wordCore(previous.word.w)
                && item.word.startSec < previous.word.endSec + 0.3) {
                // Keep whichever surface carries punctuation, and widen the
                // span to cover both so no audio is orphaned between them.
                const double from = qMin(previous.word.startSec, item.word.startSec);
                const double to = qMax(previous.word.endSec, item.word.endSec);
                previous = item;
                previous.word.startSec = from;
                previous.word.endSec = to;
                continue;
            }
        }
        collapsed.append(item);
    }
    kept = collapsed;

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
    }
    for (int index = 0; index < m_rows.size(); ++index) {
        if (m_rows.at(index).speaker == to || m_rows.at(index).speaker == from)
            rebuildPhrasesAt(index);
    }
    // A reviewer's decision outranks whatever the tier will say next, so the
    // slot map follows the rename - otherwise the next chunk's verified name
    // lands on the words that arrive after it and the meeting ends up with
    // two names for one voice.
    const QString slot = to.isEmpty() ? from : to;
    if (verifiedName.trimmed().isEmpty())
        m_nameBySlot.remove(slot);
    else
        m_nameBySlot.insert(slot, verifiedName.trimmed());
    if (!from.isEmpty() && from != slot)
        m_nameBySlot.remove(from);
    rebuildProvisional();
    if (m_haveProvisional && m_provisional.speaker == from) {
        if (!to.isEmpty())
            m_provisional.speaker = to;
        m_provisional.verifiedName = verifiedName;
    }
    ++m_revision;
    ++m_version;
}
