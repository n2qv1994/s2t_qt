#include "StatusPanel.h"

#include "Theme.h"

#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// Above this many seconds between the room and the text, the headline stops
// being green.  Both thresholds are speech-time, so a long pause in the
// meeting does not push the panel amber on its own.
const double kFreshnessOkSec = 1.5;
const double kFreshnessWarnSec = 4.0;

QString clock(double seconds)
{
    const double value = qMax(0.0, seconds);
    const int minutes = int(value) / 60;
    const int rest = int(value) % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(rest, 2, 10, QLatin1Char('0'));
}

QString secs(double value, int decimals = 2)
{
    return QStringLiteral("%1 s").arg(qMax(0.0, value), 0, 'f', decimals);
}

QString ms(double value)
{
    return QStringLiteral("%1 ms").arg(qMax(0.0, value), 0, 'f', 0);
}

enum ItemRole {
    RoleTime = Qt::UserRole + 1,
    RoleSpeaker,
    RoleConf,
    RoleText,
};

// The review list is the one place an operator reads *while* the meeting runs,
// so it gets a delegate rather than "%1 · %2 · %3%\n%4" in a plain item: the
// timestamp, the speaker and the confidence are three different questions and
// reading them apart is faster than reading them as a sentence.
class HighlightDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        const QFontMetrics head(headFont(option.font));
        const QFontMetrics body(option.font);
        // Header line, then exactly two clamped body lines, then padding.
        return QSize(80, head.height() + 2 * body.lineSpacing() + 3 * theme::kGapTight + 8);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = option.state & QStyle::State_Selected;
        const QRectF card = QRectF(option.rect).adjusted(1, 1, -1, -2);
        if (selected || (option.state & QStyle::State_MouseOver)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::color(selected ? theme::Role::AccentSoft
                                                    : theme::Role::SurfaceSunken));
            painter->drawRoundedRect(card, 6, 6);
        }

        const QString time = index.data(RoleTime).toString();
        const QString speaker = index.data(RoleSpeaker).toString();
        const int conf = index.data(RoleConf).toInt();
        const QString text = index.data(RoleText).toString();

        // A negative confidence marks the empty-state row, which has no
        // speaker, no timestamp and nothing to badge.
        if (conf < 0) {
            painter->setPen(theme::color(theme::Role::TextFaint));
            painter->drawText(QRectF(option.rect).adjusted(theme::kGap, 0, -theme::kGap, 0),
                              Qt::AlignCenter | Qt::TextWordWrap, speaker);
            painter->restore();
            return;
        }

        const QFont headText = headFont(option.font);
        const QFontMetrics head(headText);
        QRectF row = card.adjusted(theme::kGap, theme::kGapTight, -theme::kGap, 0);
        const double headBaselineHeight = head.height();

        // A coloured rule down the left edge carries the speaker identity, so
        // the name itself can stay in the reading colour.
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::laneColor(int(qHash(speaker))));
        painter->drawRoundedRect(QRectF(card.left() + 2, card.top() + 3, 2.5,
                                        card.height() - 6),
                                 1.2, 1.2);

        painter->setFont(theme::mono(-1.0));
        painter->setPen(theme::color(theme::Role::TextFaint));
        const QFontMetrics monoMetrics(painter->font());
        const double timeWidth = monoMetrics.horizontalAdvance(time) + theme::kGap;
        painter->drawText(QRectF(row.left(), row.top(), timeWidth, headBaselineHeight),
                          Qt::AlignVCenter | Qt::AlignLeft, time);

        // Confidence sits hard right: it is the reason the row is in this list
        // at all, and a fixed column lets the eye run down it.
        const QString badge = QStringLiteral("%1%").arg(conf);
        painter->setFont(headText);
        const double badgeWidth = head.horizontalAdvance(badge) + 2 * theme::kGapTight + 4;
        const QRectF badgeRect(row.right() - badgeWidth, row.top() + 1, badgeWidth,
                               headBaselineHeight - 2);
        const theme::Tone tone = conf < 50 ? theme::Tone::Danger : theme::Tone::Warn;
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::color(tone == theme::Tone::Danger ? theme::Role::DangerSoft
                                                                   : theme::Role::WarnSoft));
        painter->drawRoundedRect(badgeRect, badgeRect.height() / 2.0, badgeRect.height() / 2.0);
        painter->setPen(theme::color(tone == theme::Tone::Danger ? theme::Role::Danger
                                                                 : theme::Role::Warn));
        painter->drawText(badgeRect, Qt::AlignCenter, badge);

        painter->setPen(theme::color(theme::Role::TextMuted));
        const QRectF speakerRect(row.left() + timeWidth, row.top(),
                                 row.width() - timeWidth - badgeWidth - theme::kGapTight,
                                 headBaselineHeight);
        painter->drawText(speakerRect, Qt::AlignVCenter | Qt::AlignLeft,
                          head.elidedText(speaker, Qt::ElideRight, int(speakerRect.width())));

        // Two lines of the phrase, the second elided rather than clipped, so a
        // long sentence ends in "..." instead of half a letter.
        painter->setFont(option.font);
        painter->setPen(theme::color(theme::Role::Text));
        const QFontMetrics body(option.font);
        const double textTop = row.top() + headBaselineHeight + theme::kGapTight;
        const double width = row.width();

        QTextLayout layout(text, option.font);
        layout.beginLayout();
        QTextLine first = layout.createLine();
        int secondStart = -1;
        if (first.isValid()) {
            first.setLineWidth(width);
            QTextLine second = layout.createLine();
            if (second.isValid())
                secondStart = second.textStart();
        }
        layout.endLayout();

        if (secondStart < 0) {
            painter->drawText(QRectF(row.left(), textTop, width, body.lineSpacing()),
                              Qt::AlignLeft | Qt::AlignVCenter, text);
        } else {
            painter->drawText(QRectF(row.left(), textTop, width, body.lineSpacing()),
                              Qt::AlignLeft | Qt::AlignVCenter, text.left(secondStart).trimmed());
            painter->drawText(
                QRectF(row.left(), textTop + body.lineSpacing(), width, body.lineSpacing()),
                Qt::AlignLeft | Qt::AlignVCenter,
                body.elidedText(text.mid(secondStart), Qt::ElideRight, int(width)));
        }
        painter->restore();
    }

private:
    static QFont headFont(QFont font)
    {
        font.setPointSizeF(qMax(6.5, font.pointSizeF() - 1.0));
        font.setBold(true);
        return font;
    }
};

} // namespace

StatusPanel::StatusPanel(QWidget *parent) : QWidget(parent)
{
    // Without this the panel is transparent, the card inside it sits on the
    // same white as everything else, and the hairline border round the card
    // does no grouping work at all.
    setObjectName(QStringLiteral("s2tPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi();
}

QLabel *StatusPanel::addMetric(QGridLayout *grid, int row, const QString &caption,
                               const QString &tip)
{
    auto *name = new QLabel(caption, this);
    name->setProperty("s2tMuted", true);
    if (!tip.isEmpty())
        name->setToolTip(tip);
    auto *value = new QLabel(QStringLiteral("--"), this);
    value->setFont(theme::mono());
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (!tip.isEmpty())
        value->setToolTip(tip);
    grid->addWidget(name, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(value, row, 1, Qt::AlignRight | Qt::AlignVCenter);
    return value;
}

void StatusPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(theme::kGap, theme::kGap, theme::kGap, theme::kGap);
    outer->setSpacing(theme::kGap);

    // ---- the one number ---------------------------------------------------
    m_headlineCard = new QWidget(this);
    theme::styleCard(m_headlineCard);
    auto *cardLayout = new QVBoxLayout(m_headlineCard);
    cardLayout->setContentsMargins(theme::kPad, theme::kGap + 2, theme::kPad, theme::kPad);
    cardLayout->setSpacing(theme::kGapTight);

    auto *cardTitle = new QLabel(QStringLiteral("ĐỘ TRỄ VĂN BẢN"), m_headlineCard);
    theme::styleHeading(cardTitle);
    cardLayout->addWidget(cardTitle);

    auto *valueRow = new QHBoxLayout();
    valueRow->setSpacing(theme::kGapTight);
    m_headline = new QLabel(QStringLiteral("--"), m_headlineCard);
    {
        // Relative to the desktop font, never a pixel size: the same label is
        // measurably wider under the RHEL font stack than under MinGW.
        QFont big = m_headline->font();
        big.setPointSizeF(qMax(14.0, big.pointSizeF() * 2.5));
        big.setBold(true);
        m_headline->setFont(big);
    }
    valueRow->addWidget(m_headline, 0, Qt::AlignBottom);
    m_headlineUnit = new QLabel(QString(), m_headlineCard);
    m_headlineUnit->setProperty("s2tMuted", true);
    valueRow->addWidget(m_headlineUnit, 0, Qt::AlignBottom);
    valueRow->addStretch(1);
    cardLayout->addLayout(valueRow);

    m_headlineCaption = new QLabel(QStringLiteral("chưa có văn bản nào"), m_headlineCard);
    m_headlineCaption->setProperty("s2tMuted", true);
    m_headlineCaption->setWordWrap(true);
    cardLayout->addWidget(m_headlineCaption);

    auto *rule = new QFrame(m_headlineCard);
    rule->setFrameShape(QFrame::HLine);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("border:none; background:%1;")
                            .arg(theme::color(theme::Role::Border).name()));
    cardLayout->addSpacing(theme::kGapTight);
    cardLayout->addWidget(rule);
    cardLayout->addSpacing(theme::kGapTight);

    auto *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(theme::kGap);
    grid->setVerticalSpacing(theme::kGapTight + 1);
    grid->setColumnStretch(0, 1);
    m_backlog = addMetric(grid, 0, QStringLiteral("Tồn đọng tới AI"),
                          QStringLiteral("Audio đã thu nhưng chưa tới tầng suy luận: hàng đợi "
                                         "trên máy này cộng hàng đợi trên Server buffer."));
    m_transport = addMetric(grid, 1, QStringLiteral("ACK · chờ AI"),
                            QStringLiteral("Thời gian Server buffer xác nhận một gói audio, và "
                                           "thời gian gói đó chờ tầng suy luận."));
    m_progress = addMetric(grid, 2, QStringLiteral("Audio · thực tế"),
                           QStringLiteral("Số giây audio đã xử lý so với số giây đồng hồ đã "
                                          "trôi qua, và tỉ lệ giữa hai số đó."));
    m_coverage = addMetric(grid, 3, QStringLiteral("Tiếng nói · văn bản"),
                           QStringLiteral("Mốc thời gian của tiếng nói cuối cùng nhận được, và "
                                          "của từ cuối cùng đã hiện ra."));
    cardLayout->addLayout(grid);

    m_error = new QLabel(m_headlineCard);
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    m_error->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:6px;"
                                          "padding:6px 8px;")
                               .arg(theme::color(theme::Role::Danger).name(),
                                    theme::color(theme::Role::DangerSoft).name()));
    cardLayout->addSpacing(theme::kGapTight);
    cardLayout->addWidget(m_error);

    // ---- the engineer's half, folded away ---------------------------------
    m_detailToggle = new QToolButton(m_headlineCard);
    m_detailToggle->setText(QStringLiteral("Chi tiết kỹ thuật"));
    m_detailToggle->setCheckable(true);
    m_detailToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_detailToggle->setArrowType(Qt::RightArrow);
    m_detailToggle->setAutoRaise(true);
    m_detailToggle->setToolTip(QStringLiteral(
        "Phân vị độ trễ của máy chủ và của máy này, cùng các bộ đếm của mô hình "
        "bản chép. Dùng khi báo lỗi, không cần trong lúc điều hành."));
    cardLayout->addWidget(m_detailToggle, 0, Qt::AlignLeft);

    m_detail = new QLabel(m_headlineCard);
    m_detail->setFont(theme::mono(-1.0));
    m_detail->setWordWrap(true);
    m_detail->setVisible(false);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setProperty("s2tMuted", true);
    cardLayout->addWidget(m_detail);

    connect(m_detailToggle, &QToolButton::toggled, this, [this](bool open) {
        m_detailToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        m_detail->setVisible(open);
    });

    outer->addWidget(m_headlineCard);

    // ---- the review list --------------------------------------------------
    auto *reviewHeader = new QHBoxLayout();
    reviewHeader->setSpacing(theme::kGap);
    m_reviewTitle = new QLabel(QStringLiteral("CẦN SOÁT LẠI"), this);
    theme::styleHeading(m_reviewTitle);
    reviewHeader->addWidget(m_reviewTitle);
    reviewHeader->addStretch(1);
    m_reviewCount = new QLabel(QStringLiteral("0"), this);
    theme::stylePill(m_reviewCount, theme::Tone::Neutral);
    reviewHeader->addWidget(m_reviewCount);
    outer->addSpacing(theme::kGapTight);
    outer->addLayout(reviewHeader);

    m_highlights = new QListWidget(this);
    m_highlights->setItemDelegate(new HighlightDelegate(m_highlights));
    m_highlights->setSelectionMode(QAbstractItemView::SingleSelection);
    m_highlights->setUniformItemSizes(false);
    m_highlights->setMouseTracking(true);
    m_highlights->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_highlights->setToolTip(QStringLiteral(
        "Những cụm từ mô hình tự đánh giá là kém chắc chắn. Mở bảng \"Soát && sửa\" "
        "để nghe lại và sửa."));
    outer->addWidget(m_highlights, 1);

    setHighlights({}, 75);
}

void StatusPanel::setReadout(const StatusReadout &readout)
{
    // Headline.  While nothing has arrived the panel says so in words rather
    // than showing a confident 0.00.
    if (!readout.hasText) {
        m_headline->setText(QStringLiteral("--"));
        m_headlineUnit->setText(QString());
        m_headlineCaption->setText(readout.running
                                       ? QStringLiteral("đang chờ từ đầu tiên từ tầng suy luận")
                                       : QStringLiteral("chưa có phiên nào đang chạy"));
        m_headline->setStyleSheet(QStringLiteral("color:%1;")
                                      .arg(theme::color(theme::Role::TextFaint).name()));
    } else {
        const double lag = readout.freshnessSec;
        const theme::Role tint = lag <= kFreshnessOkSec  ? theme::Role::Ok
            : lag <= kFreshnessWarnSec                   ? theme::Role::Warn
                                                         : theme::Role::Danger;
        m_headline->setText(QString::number(lag, 'f', 2));
        m_headline->setStyleSheet(QStringLiteral("color:%1;").arg(theme::color(tint).name()));
        m_headlineUnit->setText(readout.accelerated ? QStringLiteral("giây audio")
                                                    : QStringLiteral("giây"));
        QString caption = readout.done
            ? QStringLiteral("phiên đã kết thúc")
            : QStringLiteral("văn bản chậm hơn lời nói");
        if (readout.accelerated) {
            caption += QStringLiteral(" · ≈%1 s thực tế")
                           .arg(lag / qMax(1.0, readout.wallScale), 0, 'f', 2);
        }
        m_headlineCaption->setText(caption);
    }

    if (readout.haveBacklog) {
        const double total = readout.backlogLocalSec + readout.backlogServerSec;
        m_backlog->setText(secs(total));
        m_backlog->setToolTip(QStringLiteral("máy này %1 · server %2")
                                  .arg(secs(readout.backlogLocalSec),
                                       secs(readout.backlogServerSec)));
        m_transport->setText(QStringLiteral("%1 · %2")
                                 .arg(ms(readout.ackLastMs), ms(readout.aiWaitMs)));
        m_transport->setToolTip(QStringLiteral("ACK lớn nhất từ đầu phiên: %1")
                                    .arg(ms(readout.ackMaxMs)));
    } else {
        m_backlog->setText(QStringLiteral("--"));
        m_transport->setText(QStringLiteral("--"));
    }

    m_progress->setText(QStringLiteral("%1 · %2 · %3")
                            .arg(clock(readout.audioSec), clock(readout.wallSec),
                                 readout.speed > 0
                                     ? QStringLiteral("%1×").arg(readout.speed, 0, 'f', 2)
                                     : QStringLiteral("--")));
    m_coverage->setText(QStringLiteral("%1 · %2")
                            .arg(clock(readout.speechSec),
                                 readout.textSec > 0 ? clock(readout.textSec)
                                                     : QStringLiteral("--:--")));

    m_error->setVisible(!readout.error.isEmpty());
    m_error->setText(readout.error);

    m_detail->setText(readout.detail.join(QLatin1Char('\n')));
}

void StatusPanel::setHighlights(const QList<asr::Highlight> &items, int thresholdPct)
{
    m_reviewTitle->setText(QStringLiteral("CẦN SOÁT LẠI · DƯỚI %1%").arg(thresholdPct));

    // Rebuild only on an actual change.  Comparing counts alone would miss a
    // correction that rewrote a highlight's text without adding one, which is
    // exactly the case this panel exists to show.
    QString renderKey;
    for (const asr::Highlight &highlight : items) {
        renderKey += QStringLiteral("%1|%2|%3|%4\n")
                         .arg(highlight.startSec, 0, 'f', 2)
                         .arg(highlight.speaker)
                         .arg(highlight.confPct)
                         .arg(highlight.text);
    }
    if (renderKey == m_highlightsKey)
        return;
    m_highlightsKey = renderKey;

    m_reviewCount->setText(QString::number(items.size()));
    theme::stylePill(m_reviewCount,
                     items.isEmpty() ? theme::Tone::Neutral
                                     : (items.size() > 20 ? theme::Tone::Danger
                                                          : theme::Tone::Warn));

    m_highlights->clear();
    if (items.isEmpty()) {
        // An empty list widget is indistinguishable from a broken one, so say
        // which of the two this is.
        auto *empty = new QListWidgetItem(m_highlights);
        empty->setData(RoleTime, QString());
        empty->setData(RoleSpeaker,
                       QStringLiteral("Chưa có cụm từ nào dưới ngưỡng tin cậy."));
        empty->setData(RoleConf, -1);
        empty->setData(RoleText, QString());
        empty->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const asr::Highlight &highlight : items) {
        auto *item = new QListWidgetItem(m_highlights);
        item->setData(RoleTime, clock(highlight.startSec));
        item->setData(RoleSpeaker, highlight.speaker);
        item->setData(RoleConf, highlight.confPct);
        item->setData(RoleText, highlight.text);
        item->setToolTip(highlight.text);
    }
}
