#include "TranscriptModel.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

// Rows older than this fall out of the local cache, and no more than this
// many are kept - a long meeting must not grow the client without bound.
const double kLiveRowCacheSec = 15 * 60;
const int kLiveRowCacheMaxRows = 180;

const int kLaneLockMax = 6000;
const int kDisplayLockMax = 8000;

bool hasVerifiedName(const QString &name)
{
    const QString value = name.trimmed().toLower();
    if (value.isEmpty())
        return false;
    static const QSet<QString> placeholders = {
        QStringLiteral("unknown"), QStringLiteral("unk"), QStringLiteral("?"),
        QStringLiteral("spk?"), QStringLiteral("speaker?"),
    };
    if (placeholders.contains(value))
        return false;
    if (value.startsWith(QStringLiteral("unknown_")) || value.startsWith(QStringLiteral("unknown-")))
        return false;
    static const QRegularExpression bare(QStringLiteral("^speaker_\\d+$"));
    return !bare.match(value).hasMatch();
}

int speakerIndexOf(const asr::DisplayRow &row)
{
    bool ok = false;
    const int value = row.speaker.toInt(&ok);
    return (ok && value >= 0) ? value : 0;
}

QString laneKeyFor(const asr::DisplayRow &row)
{
    // Once verification produces a real identity, every diar cluster verified
    // as that identity must share one lane (sid:0 -> p8 and sid:1 -> p8 are
    // the same person).  Until then the diar slot is the only stable key.
    if (hasVerifiedName(row.verifiedName))
        return QStringLiteral("name:") + row.verifiedName.trimmed().toLower();
    return QStringLiteral("sid:") + QString::number(speakerIndexOf(row));
}

QString laneLabelFor(const asr::DisplayRow &row)
{
    if (hasVerifiedName(row.verifiedName))
        return row.verifiedName.trimmed();
    return QStringLiteral("speaker_") + QString::number(speakerIndexOf(row));
}

bool isVerifiedLaneKey(const QString &key)
{
    return key.startsWith(QStringLiteral("name:"));
}

bool isUnverifiedLaneKey(const QString &key)
{
    return key.startsWith(QStringLiteral("sid:")) || key.startsWith(QStringLiteral("pending:"));
}

QString slotKeyFor(double startSec, double endSec)
{
    const double s = qMax(0.0, startSec);
    const double e0 = qMax(s, endSec);
    const double e = e0 > s ? e0 : s + 0.01;
    // Bucket the centre at 20 ms and the duration at 10 ms: small timestamp
    // jitter between two passes must not create a second logical token.
    const qint64 centre = qint64(std::llround((s + e) * 0.5 * 50.0));
    const qint64 duration = qint64(std::llround((e - s) * 100.0));
    return QString::number(centre) + QLatin1Char('|') + QString::number(duration);
}

int punctuationScore(const QString &text)
{
    int score = 0;
    for (const QChar &ch : text) {
        if (QStringLiteral(".,;:!?").contains(ch))
            score += 2;
    }
    return score;
}

bool shouldUpgradeLockedText(const QString &previous, const QString &next)
{
    if (next.isEmpty() || previous == next)
        return false;
    if (TranscriptModel::normalizeForSlot(previous) != TranscriptModel::normalizeForSlot(next))
        return false;
    return punctuationScore(next) > punctuationScore(previous);
}

double overlapRatio(const WordItem &a, const WordItem &b)
{
    const double as = qMax(0.0, a.startSec);
    const double ae = qMax(as, a.endSec);
    const double bs = qMax(0.0, b.startSec);
    const double be = qMax(bs, b.endSec);
    const double overlap = qMax(0.0, qMin(ae, be) - qMax(as, bs));
    const double denominator = qMax(0.01, qMin(qMax(0.01, ae - as), qMax(0.01, be - bs)));
    return overlap / denominator;
}

QStringList splitSurfaceTokens(const QString &text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return text.trimmed().split(whitespace, Qt::SkipEmptyParts);
}

// The token streams carry raw words while merged_text/phrase text carries the
// punctuated surface form.  When the two agree word for word, prefer the
// punctuated labels - that is what makes the timeline read like a sentence
// instead of a list of bare tokens.
QList<WordItem> relabelFromSurface(const QList<WordItem> &words, const asr::DisplayRow &row)
{
    if (words.isEmpty())
        return words;

    QStringList candidates;
    QStringList phraseParts;
    for (const asr::Phrase &phrase : row.phrases) {
        const QString text = phrase.text.trimmed();
        if (!text.isEmpty())
            phraseParts << text;
    }
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const QString phraseText = phraseParts.join(QLatin1Char(' '));
    for (const QString &candidate : {phraseText, row.mergedText, row.updatingText}) {
        const QString cleaned = QString(candidate).replace(whitespace, QStringLiteral(" ")).trimmed();
        if (!cleaned.isEmpty() && !candidates.contains(cleaned))
            candidates << cleaned;
    }

    QStringList baseCores;
    baseCores.reserve(words.size());
    int currentScore = 0;
    for (const WordItem &word : words) {
        baseCores << TranscriptModel::normalizeForSlot(word.text);
        currentScore += punctuationScore(word.text);
    }

    QStringList bestLabels;
    int bestScore = 0;
    for (const QString &surface : candidates) {
        const QStringList tokens = splitSurfaceTokens(surface);
        if (tokens.isEmpty() || tokens.size() < baseCores.size())
            continue;
        QStringList cores;
        cores.reserve(tokens.size());
        for (const QString &token : tokens)
            cores << TranscriptModel::normalizeForSlot(token);

        int start = -1;
        for (int offset = 0; offset + baseCores.size() <= cores.size(); ++offset) {
            bool matched = true;
            for (int i = 0; i < baseCores.size(); ++i) {
                if (cores.at(offset + i) != baseCores.at(i)) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                start = offset;
                break;
            }
        }
        if (start < 0)
            continue;
        const QStringList labels = tokens.mid(start, baseCores.size());
        int score = 0;
        for (const QString &label : labels)
            score += punctuationScore(label);
        score -= currentScore;
        if (bestLabels.isEmpty() || score > bestScore) {
            bestLabels = labels;
            bestScore = score;
        }
    }
    if (bestLabels.isEmpty() || bestScore <= 0)
        return words;

    QList<WordItem> out = words;
    for (int i = 0; i < out.size() && i < bestLabels.size(); ++i) {
        if (!bestLabels.at(i).isEmpty())
            out[i].text = bestLabels.at(i);
    }
    return out;
}

QString stableRowKey(const asr::DisplayRow &row)
{
    if (!row.rowId.trimmed().isEmpty())
        return row.rowId.trimmed();
    return QStringLiteral("stable:%1:%2")
        .arg(row.speaker.isEmpty() ? QStringLiteral("?") : row.speaker)
        .arg(qint64(std::llround(row.startSec * 1000.0)));
}

} // namespace

QString TranscriptModel::normalizeForSlot(const QString &text)
{
    QString value = text.trimmed().toLower().normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(value.size());
    for (const QChar &ch : value) {
        const ushort code = ch.unicode();
        if (code >= 0x0300 && code <= 0x036f)
            continue; // combining diacritic
        if (QStringLiteral(".,;:!?'\"`~()[]{}<>").contains(ch))
            continue;
        out.append(ch);
    }
    return out;
}

void TranscriptModel::resetForSession(const QString &sessionKey)
{
    m_sessionId = sessionKey;
    m_state = asr::SessionState();
    m_rows.clear();
    m_lanes.clear();
    m_revision = 0;
    m_final = false;
    m_review = false;
    m_commitBoundarySec = -1.0;
    m_latestTextEndSec = 0.0;
    m_rowCache.clear();
    m_rowCacheSession = sessionKey;
    m_laneMeta.clear();
    m_laneOrderCounter = 0;
    m_laneLocks.clear();
    m_displayLocks.clear();
}

void TranscriptModel::setShowLowConfidence(bool show)
{
    if (m_showLowConfidence == show)
        return;
    m_showLowConfidence = show;
    // Display locks captured which words were visible under the old
    // threshold; drop them so newly admitted words are not held out by a
    // snapshot taken while they were being filtered away.
    m_displayLocks.clear();
    rebuild();
}

double TranscriptModel::totalSec() const
{
    double maxEnd = 0.0;
    for (const asr::DisplayRow &row : m_rows)
        maxEnd = qMax(maxEnd, row.endSec);
    for (const asr::DisplayRow &row : m_state.provisionalRows)
        maxEnd = qMax(maxEnd, row.endSec);
    return qMax(qMax(15.0, maxEnd + 0.5), qMax(m_state.sourceTotalSec, m_state.sourceSeenSec));
}

QList<WordItem> TranscriptModel::rowWordItems(const asr::DisplayRow &row)
{
    QList<WordItem> out;
    const double rowStart = std::isfinite(row.startSec) ? qMax(0.0, row.startSec) : 0.0;
    const double rowEndRaw = std::isfinite(row.endSec) ? row.endSec : rowStart;
    const double rowEnd = rowEndRaw > rowStart ? rowEndRaw : rowStart + 0.12;

    const auto estimateStep = [&](int count) {
        const double span = qMax(0.08, rowEnd - rowStart);
        // If a token stream arrives without timestamps, spread it across the
        // row instead of collapsing every word onto the same pixel.
        return qMax(0.12, span / double(qMax(1, count)));
    };
    const auto push = [&](const QString &text, double start, double end, double conf) {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty() || !std::isfinite(start) || !std::isfinite(end))
            return;
        WordItem item;
        item.text = trimmed;
        item.startSec = qMax(0.0, start);
        item.endSec = end > item.startSec ? end : item.startSec + 0.01;
        item.conf = float(qBound(0.0, conf, 1.0));
        out.append(item);
    };
    const auto pushEstimated = [&](const QString &text, double conf, int index, int count) {
        const double step = estimateStep(count);
        const double start = rowStart + double(qMax(0, index)) * step;
        push(text, start, start + step, conf);
    };

    const auto fromWordList = [&](const QList<asr::Word> &words) {
        for (int i = 0; i < words.size(); ++i) {
            const asr::Word &word = words.at(i);
            if (std::isfinite(word.startSec) && std::isfinite(word.endSec)
                && (word.startSec > 0.0 || word.endSec > 0.0))
                push(word.w, word.startSec, word.endSec, double(word.c));
            else
                pushEstimated(word.w, double(word.c), i, words.size());
        }
    };
    const auto fromPhrases = [&]() {
        QList<asr::Word> flat;
        for (const asr::Phrase &phrase : row.phrases)
            flat.append(phrase.words);
        fromWordList(flat);
    };

    // Deterministic order.  A committed row's authoritative text is its
    // display tokens; a provisional row's is whatever is still updating.
    if (row.isProvisional) {
        fromWordList(row.updatingTokens);
        if (out.isEmpty())
            fromWordList(row.displayTokens);
        if (out.isEmpty())
            fromPhrases();
    } else {
        fromWordList(row.displayTokens);
        if (out.isEmpty())
            fromPhrases();
        if (out.isEmpty())
            fromWordList(row.updatingTokens);
    }
    if (out.isEmpty())
        return out;

    out = relabelFromSurface(out, row);

    // Same word, same span, twice (two passes over one region): keep the more
    // confident copy.
    QHash<QString, WordItem> deduped;
    for (const WordItem &item : out) {
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(qint64(std::llround(item.startSec * 1000.0)))
                                .arg(qint64(std::llround(item.endSec * 1000.0)))
                                .arg(item.text.toLower());
        const auto existing = deduped.constFind(key);
        if (existing == deduped.constEnd() || item.conf > existing->conf)
            deduped.insert(key, item);
    }
    QList<WordItem> result = deduped.values();
    std::sort(result.begin(), result.end(), [](const WordItem &a, const WordItem &b) {
        if (std::abs(a.startSec - b.startSec) > 1e-6)
            return a.startSec < b.startSec;
        return a.endSec < b.endSec;
    });
    return result;
}

void TranscriptModel::assignSlotKeys(QList<WordItem> *words) const
{
    struct Slot
    {
        QString key;
        double centre = 0.0;
        QString normalized;
        WordItem reference;
    };
    // Not named `slots`: that is a Qt keyword macro and expands to nothing.
    QList<Slot> clusters;

    QList<int> order;
    order.reserve(words->size());
    for (int i = 0; i < words->size(); ++i)
        order.append(i);
    std::sort(order.begin(), order.end(), [words](int a, int b) {
        const WordItem &wa = words->at(a);
        const WordItem &wb = words->at(b);
        return (wa.startSec + wa.endSec) * 0.5 < (wb.startSec + wb.endSec) * 0.5;
    });

    for (int index : order) {
        WordItem &word = (*words)[index];
        const double centre = (word.startSec + word.endSec) * 0.5;
        const QString normalized = normalizeForSlot(word.text);
        int best = -1;
        double bestScore = -1.0;
        for (int i = 0; i < clusters.size(); ++i) {
            const Slot &slot = clusters.at(i);
            const double centreDelta = std::abs(centre - slot.centre);
            if (centreDelta > 0.28)
                continue;
            const double overlap = overlapRatio(word, slot.reference);
            const bool sameText = !normalized.isEmpty() && normalized == slot.normalized;
            if (!sameText && overlap < 0.35)
                continue;
            const double score = (sameText ? 2.0 : 0.0) + overlap - centreDelta;
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best >= 0) {
            word.slotKey = clusters.at(best).key;
            Slot &slot = clusters[best];
            // Keep the committed / more confident copy as the representative
            // so later matching compares against the better evidence.
            if ((slot.reference.provisional && !word.provisional) || word.conf > slot.reference.conf) {
                slot.reference = word;
                slot.centre = centre;
                if (!normalized.isEmpty())
                    slot.normalized = normalized;
            }
            continue;
        }
        Slot slot;
        slot.key = slotKeyFor(word.startSec, word.endSec);
        slot.centre = centre;
        slot.normalized = normalized;
        slot.reference = word;
        clusters.append(slot);
        word.slotKey = slot.key;
    }
}

void TranscriptModel::mergeLiveRows(const asr::SessionState &incoming)
{
    if (m_rowCacheSession != m_sessionId) {
        m_rowCacheSession = m_sessionId;
        m_rowCache.clear();
    }

    if (!incoming.rows.isEmpty()) {
        QSet<QString> incomingKeys;
        double authoritativeStart = incoming.rows.first().startSec;
        for (const asr::DisplayRow &row : incoming.rows) {
            incomingKeys.insert(stableRowKey(row));
            authoritativeStart = qMin(authoritativeStart, row.startSec);
        }
        // The response is authoritative for its whole bounded tail, and only
        // for that: a cached row inside the tail that the server no longer
        // reports has genuinely been withdrawn, while older rows outside it
        // stay mounted rather than being rebuilt on every poll.
        for (auto it = m_rowCache.begin(); it != m_rowCache.end();) {
            if (it.value().endSec > authoritativeStart + 0.001 && !incomingKeys.contains(it.key()))
                it = m_rowCache.erase(it);
            else
                ++it;
        }
        for (const asr::DisplayRow &row : incoming.rows)
            m_rowCache.insert(stableRowKey(row), row);
    }

    const double keepAfter = qMax(0.0, incoming.sourceSeenSec - kLiveRowCacheSec);
    for (auto it = m_rowCache.begin(); it != m_rowCache.end();) {
        if (it.value().endSec < keepAfter)
            it = m_rowCache.erase(it);
        else
            ++it;
    }

    m_rows = m_rowCache.values();
    std::sort(m_rows.begin(), m_rows.end(),
              [](const asr::DisplayRow &a, const asr::DisplayRow &b) {
                  if (std::abs(a.startSec - b.startSec) > 1e-6)
                      return a.startSec < b.startSec;
                  return a.endSec < b.endSec;
              });
    if (m_rows.size() > kLiveRowCacheMaxRows) {
        const int excess = m_rows.size() - kLiveRowCacheMaxRows;
        for (int i = 0; i < excess; ++i)
            m_rowCache.remove(stableRowKey(m_rows.at(i)));
        m_rows.remove(0, excess);
    }
}

void TranscriptModel::applyLiveState(const asr::StateResponse &response)
{
    if (response.sessionId != m_sessionId)
        resetForSession(response.sessionId);
    m_state = response.state;
    m_revision = response.transcriptRevision;
    m_final = response.transcriptFinal;
    m_commitBoundarySec = response.commitBoundarySec;
    m_review = false;
    mergeLiveRows(response.state);
    rebuild();
}

void TranscriptModel::applyReviewState(const asr::StateResponse &response)
{
    if (response.sessionId != m_sessionId)
        resetForSession(response.sessionId);
    m_state = response.state;
    m_revision = response.transcriptRevision;
    m_final = response.transcriptFinal;
    m_commitBoundarySec = response.commitBoundarySec;
    m_review = true;
    m_rows = response.state.rows;
    rebuild();
}

void TranscriptModel::applyEditedState(const asr::SessionState &state, quint64 revision)
{
    m_state = state;
    m_revision = revision;
    if (m_review) {
        m_rows = state.rows;
    } else {
        mergeLiveRows(state);
    }
    rebuild();
}

void TranscriptModel::rebuild()
{
    const double dropThreshold =
        m_showLowConfidence ? 0.0 : qBound(0.0, kDefaultDropThreshold, 1.0);
    const double warnThreshold =
        qMax(dropThreshold, qBound(0.0, double(m_state.confThresholdPct) / 100.0, 1.0));

    QList<asr::DisplayRow> renderRows = m_rows;
    renderRows.append(m_state.provisionalRows);

    // A diar lane created before verification returned an identity is now
    // superseded; drop the alias or the same person shows up twice.
    QHash<int, QString> verifiedBySpeakerIndex;
    for (const asr::DisplayRow &row : renderRows) {
        if (hasVerifiedName(row.verifiedName))
            verifiedBySpeakerIndex.insert(speakerIndexOf(row), laneKeyFor(row));
    }
    for (auto it = m_laneMeta.begin(); it != m_laneMeta.end();) {
        static const QRegularExpression sidPattern(QStringLiteral("^sid:(\\d+)$"));
        const auto match = sidPattern.match(it.key());
        if (match.hasMatch() && verifiedBySpeakerIndex.contains(match.captured(1).toInt()))
            it = m_laneMeta.erase(it);
        else
            ++it;
    }

    // Keep lanes the session registry knows about even when the current tail
    // has no words from them, so a quiet speaker's row does not vanish and
    // shuffle every other lane up.
    for (const QString &rawId : m_state.speakerIds) {
        bool ok = false;
        const int index = rawId.toInt(&ok);
        if (!ok || index < 0)
            continue;
        // ...but never re-create a diar slot that verification has already
        // superseded.  speaker_ids always lists every slot, so without this
        // the erase above is undone on the very next line and the same person
        // shows up twice: once as "speaker_1" with no words, once under their
        // real name.
        if (verifiedBySpeakerIndex.contains(index))
            continue;
        const QString key = QStringLiteral("sid:") + QString::number(index);
        if (!m_laneMeta.contains(key)) {
            LaneMeta meta;
            meta.index = m_laneOrderCounter++;
            meta.speakerIndex = index;
            meta.label = QStringLiteral("speaker_") + QString::number(index);
            m_laneMeta.insert(key, meta);
        }
    }

    QList<WordItem> flat;
    m_latestTextEndSec = 0.0;
    for (const asr::DisplayRow &row : renderRows) {
        const QString rowLane = laneKeyFor(row);
        const QString rowLabel = laneLabelFor(row);
        if (!m_laneMeta.contains(rowLane)) {
            LaneMeta meta;
            meta.index = m_laneOrderCounter++;
            meta.speakerIndex = speakerIndexOf(row);
            meta.label = rowLabel;
            m_laneMeta.insert(rowLane, meta);
        } else {
            LaneMeta &meta = m_laneMeta[rowLane];
            meta.speakerIndex = speakerIndexOf(row);
            if (hasVerifiedName(row.verifiedName))
                meta.label = row.verifiedName.trimmed();
        }
        const QList<WordItem> items = rowWordItems(row);
        for (const WordItem &item : items) {
            m_latestTextEndSec = qMax(m_latestTextEndSec, item.endSec);
            if (dropThreshold > 0.0 && double(item.conf) < dropThreshold)
                continue;
            WordItem word = item;
            word.provisional = row.isProvisional;
            word.low = warnThreshold > 0.0 && double(word.conf) < warnThreshold;
            word.laneKey = rowLane;
            flat.append(word);
        }
        if (items.isEmpty())
            m_latestTextEndSec = qMax(m_latestTextEndSec, row.endSec);
    }

    assignSlotKeys(&flat);

    // Lane lock.  Provisional ASR can carry noisy diar labels; the first
    // committed row is allowed to correct the lane once, then it is frozen -
    // except for a single further upgrade from a diar slot to a verified
    // identity, which is never allowed to run backwards.
    for (WordItem &word : flat) {
        const QString slot = word.slotKey.isEmpty() ? slotKeyFor(word.startSec, word.endSec)
                                                    : word.slotKey;
        word.slotKey = slot;
        const auto existing = m_laneLocks.constFind(slot);
        if (existing == m_laneLocks.constEnd()) {
            m_laneLocks.insert(slot, {word.laneKey, !word.provisional});
        } else if (!existing->committed && !word.provisional) {
            m_laneLocks.insert(slot, {word.laneKey, true});
        } else if (existing->committed && !word.provisional
                   && isUnverifiedLaneKey(existing->laneKey) && isVerifiedLaneKey(word.laneKey)) {
            m_laneLocks.insert(slot, {word.laneKey, true});
        } else {
            word.laneKey = existing->laneKey;
        }
    }
    // Bound by dropping locks for slots that are no longer on screen, never by
    // clearing wholesale: a cleared lane lock lets already-committed words be
    // re-assigned by a later diarization pass, which is the exact jumping this
    // lock exists to prevent.
    if (m_laneLocks.size() > kLaneLockMax) {
        QSet<QString> live;
        live.reserve(flat.size());
        for (const WordItem &word : flat)
            live.insert(word.slotKey);
        for (auto it = m_laneLocks.begin(); it != m_laneLocks.end();)
            it = live.contains(it.key()) ? std::next(it) : m_laneLocks.erase(it);
    }

    // Display lock, per slot.  A slot has two phases: first-seen snapshot,
    // then one committed update, then frozen - which is what stops the
    // timeline rewriting itself on every provisional refinement.
    QHash<QString, QList<WordItem>> byLane;
    QHash<QString, QSet<QString>> seenSlotsByLane;
    for (const WordItem &word : flat) {
        const auto locked = m_displayLocks.constFind(word.slotKey);
        WordItem display = word;
        if (locked != m_displayLocks.constEnd() && locked->committed) {
            WordItem snapshot = locked->snapshot;
            const bool revisionAdvanced = m_revision > locked->revision;
            const bool textChanged = snapshot.text != word.text;
            if (!word.provisional
                && (shouldUpgradeLockedText(snapshot.text, word.text)
                    || (revisionAdvanced && textChanged))) {
                snapshot.text = word.text;
                snapshot.conf = qMax(snapshot.conf, word.conf);
                snapshot.startSec = word.startSec;
                snapshot.endSec = word.endSec;
                snapshot.provisional = false;
                snapshot.low = word.low;
                m_displayLocks.insert(word.slotKey, {snapshot, true, m_revision});
            } else if (revisionAdvanced) {
                // Mark the slot as observed at the new revision even when its
                // text did not change, so a later ordinary poll cannot reuse
                // the same revision as permission for an unrelated rewrite.
                m_displayLocks.insert(word.slotKey, {snapshot, true, m_revision});
            }
            display = snapshot;
        } else if (locked == m_displayLocks.constEnd()) {
            m_displayLocks.insert(word.slotKey, {word, !word.provisional, m_revision});
        } else if (!word.provisional) {
            m_displayLocks.insert(word.slotKey, {word, true, m_revision});
        } else {
            display = locked->snapshot;
        }
        display.slotKey = word.slotKey;
        display.laneKey = word.laneKey;
        // One entry per slot per lane: a slot seen twice in one pass (two
        // rows covering the same instant) must not draw the word twice.
        if (seenSlotsByLane[word.laneKey].contains(word.slotKey))
            continue;
        seenSlotsByLane[word.laneKey].insert(word.slotKey);
        byLane[word.laneKey].append(display);
    }
    // Same reasoning as the lane lock: dropping a display lock for a word
    // still on screen un-freezes its text and lets provisional refinements
    // start rewriting it again.
    if (m_displayLocks.size() > kDisplayLockMax) {
        QSet<QString> live;
        live.reserve(flat.size());
        for (const WordItem &word : flat)
            live.insert(word.slotKey);
        for (auto it = m_displayLocks.begin(); it != m_displayLocks.end();)
            it = live.contains(it.key()) ? std::next(it) : m_displayLocks.erase(it);
    }

    QStringList orderedKeys = m_laneMeta.keys();
    for (auto it = byLane.constBegin(); it != byLane.constEnd(); ++it) {
        if (!m_laneMeta.contains(it.key())) {
            LaneMeta meta;
            meta.index = m_laneOrderCounter++;
            meta.label = it.key();
            m_laneMeta.insert(it.key(), meta);
            orderedKeys << it.key();
        }
    }
    std::sort(orderedKeys.begin(), orderedKeys.end(), [this](const QString &a, const QString &b) {
        return m_laneMeta.value(a).index < m_laneMeta.value(b).index;
    });

    m_lanes.clear();
    for (const QString &key : orderedKeys) {
        const LaneMeta meta = m_laneMeta.value(key);
        Lane lane;
        lane.key = key;
        lane.label = meta.label.isEmpty() ? key : meta.label;
        lane.colorIndex = std::abs(meta.speakerIndex) % 4;
        lane.words = byLane.value(key);
        std::sort(lane.words.begin(), lane.words.end(),
                  [](const WordItem &a, const WordItem &b) { return a.startSec < b.startSec; });
        // A lane is "live" only when its newest word is still provisional.
        lane.provisional = !lane.words.isEmpty() && lane.words.last().provisional;
        m_lanes.append(lane);
    }
}

bool TranscriptModel::rowCovering(double startSec, double endSec, asr::DisplayRow *out) const
{
    double bestOverlap = 0.0;
    bool found = false;
    const auto consider = [&](const QList<asr::DisplayRow> &rows) {
        for (const asr::DisplayRow &row : rows) {
            const double overlap = qMin(endSec, row.endSec) - qMax(startSec, row.startSec);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                *out = row;
                found = true;
            }
        }
    };
    consider(m_rows);
    consider(m_state.provisionalRows);
    return found;
}

QStringList TranscriptModel::speakerIds() const
{
    QStringList out;
    const auto collect = [&](const QList<asr::DisplayRow> &rows) {
        for (const asr::DisplayRow &row : rows) {
            if (!row.speaker.isEmpty() && !out.contains(row.speaker))
                out << row.speaker;
        }
    };
    collect(m_rows);
    collect(m_state.provisionalRows);
    std::sort(out.begin(), out.end());
    return out;
}
