#ifndef UI_STATUSPANEL_H
#define UI_STATUSPANEL_H

// The right-hand column of the recording window.
//
// It replaces two QLabels that between them rendered eleven measurements as
// one run-on block of 11px monospace - `rows=0 prov=0 phrases=0 low=0 lanes=0
// virt=0/0 [LIVE]` above `audio=00:00.00 wall=00:00.00 speed=--x`.  Everything
// was the same size, the same colour and the same weight, so the one number an
// operator has to react to (how far the text is behind the room) was no more
// visible than the lane count.
//
// The split here is by *who asks the question*:
//
//   - the headline           : is the transcript keeping up?          (operator)
//   - four metric rows       : if not, where is the time going?       (operator)
//   - "Chi tiết kỹ thuật"    : percentiles, model counters            (engineer)
//   - the review list        : which words need a human?              (operator)
//
// The engineer's half is collapsed by default and remembers nothing: it is a
// diagnostic, not a preference.

#include "proto/AsrSession.h"

#include <QList>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QGridLayout;
class QLabel;
class QListWidget;
class QToolButton;
QT_END_NAMESPACE

// Everything the panel draws, computed by the caller.  Passing one struct
// rather than a dozen setters keeps a refresh atomic: the panel never paints
// a new lag next to a stale backlog.
struct StatusReadout
{
    bool hasText = false;         // any word has arrived yet
    bool running = false;         // a session is open
    bool done = false;            // the pipeline says this session is finished

    double freshnessSec = 0.0;    // speech-time gap between the room and the text
    double rawDelaySec = 0.0;     // the same gap including trailing VAD silence
    double wallScale = 1.0;       // >1 while a file replays faster than realtime
    bool accelerated = false;

    bool haveBacklog = false;
    double backlogLocalSec = 0.0;
    double backlogServerSec = 0.0;

    double ackLastMs = 0.0;
    double ackMaxMs = 0.0;
    double aiWaitMs = 0.0;

    double audioSec = 0.0;
    double wallSec = 0.0;
    double speed = 0.0;
    double speechSec = 0.0;
    double textSec = 0.0;

    QStringList detail;           // pre-formatted engineer lines
    QString error;                // shown as a strip; empty means no strip
};

class StatusPanel : public QWidget
{
    Q_OBJECT

public:
    explicit StatusPanel(QWidget *parent = nullptr);

    void setReadout(const StatusReadout &readout);
    void setHighlights(const QList<asr::Highlight> &items, int thresholdPct);

private:
    void buildUi();
    QLabel *addMetric(QGridLayout *grid, int row, const QString &caption,
                      const QString &tip);

    QLabel *m_headline = nullptr;
    QLabel *m_headlineUnit = nullptr;
    QLabel *m_headlineCaption = nullptr;
    QWidget *m_headlineCard = nullptr;

    QLabel *m_backlog = nullptr;
    QLabel *m_transport = nullptr;
    QLabel *m_progress = nullptr;
    QLabel *m_coverage = nullptr;

    QToolButton *m_detailToggle = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_error = nullptr;

    QLabel *m_reviewTitle = nullptr;
    QLabel *m_reviewCount = nullptr;
    QListWidget *m_highlights = nullptr;
    // Deliberately not empty: an empty key would match the empty list on the
    // very first call, the early-out would fire, and the "nothing to review"
    // row would never be built - leaving a blank white box that reads as a
    // broken panel rather than as good news.
    QString m_highlightsKey = QStringLiteral("(chưa dựng)");
};

#endif // UI_STATUSPANEL_H
