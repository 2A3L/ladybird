/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/FeedbackPopover.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Localization.h>
#include <UI/Qt/Theme.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_WIDTH = 300;
static constexpr int ROW_HEIGHT = 32;
static constexpr int ROW_WITH_DETAIL_HEIGHT = 46;
static constexpr int CRASH_REPORTS_DOT_SIZE = 7;
static constexpr int CRASH_REPORTS_DOT_GAP = 6;
static constexpr int EXTERNAL_LINK_ICON_SIZE = 14;
static constexpr int CHEVRON_ICON_SIZE = 12;

static QLabel* create_row_label(QString const& text, QWidget& row)
{
    auto* label = new QLabel(text, &row);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

static QLabel* create_detail_label(QString const& text, QWidget& row)
{
    auto* label = create_row_label(text, row);
    label->setObjectName("LadybirdFeedbackDetail");
    return label;
}

static QLabel* create_detail_label(WebView::UIText text, QWidget& row)
{
    auto* label = create_detail_label(QString {}, row);
    bind_ui_text(*label, AK::move(text), [label](QString const& text) {
        label->setText(text);
    });
    return label;
}

static QLabel* create_row_icon(QWidget& row, int size)
{
    auto* icon = new QLabel(&row);
    icon->setFixedSize(size, size);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    return icon;
}

static QPushButton* create_row(QWidget& parent, WebView::UIText accessible_name, int height)
{
    auto* row = new QPushButton(&parent);
    row->setObjectName("LadybirdPopoverRow");
    bind_ui_text(*row, AK::move(accessible_name), [row](QString const& text) {
        row->setAccessibleName(text);
    });
    row->setFixedHeight(height);
    row->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(10);
    return row;
}

static QVBoxLayout* create_row_text(QPushButton& row, WebView::UIText title)
{
    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    text->setAlignment(Qt::AlignVCenter);
    auto* label = create_row_label({}, row);
    bind_ui_text(*label, AK::move(title), [label](QString const& text) {
        label->setText(text);
    });
    text->addWidget(label);
    static_cast<QHBoxLayout*>(row.layout())->addLayout(text, 1);
    return text;
}

FeedbackPopover::FeedbackPopover(QWidget* parent)
    : Popover(parent)
{
    bind_ui_text(*this, WebView::ui_text("Send feedback"sv), [this](QString const& text) {
        setAccessibleName(text);
    });
    card().setObjectName("LadybirdFeedbackPopover");
    card().setFixedWidth(POPOVER_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);

    m_report_site_row = create_row(card(), WebView::ui_text("Report a problem with this site"sv), ROW_WITH_DETAIL_HEIGHT);
    auto* report_site_text = create_row_text(*m_report_site_row, WebView::ui_text("Report a problem with this site"sv));
    m_host_label = create_detail_label(QString {}, *m_report_site_row);
    report_site_text->addWidget(m_host_label);
    QObject::connect(m_report_site_row, &QPushButton::clicked, this, [this] {
        close();
        if (on_report_site)
            on_report_site();
    });
    layout->addWidget(m_report_site_row);

    auto* report_bug_row = create_row(card(), WebView::ui_text("Report a bug on GitHub"sv), ROW_WITH_DETAIL_HEIGHT);
    auto* report_bug_text = create_row_text(*report_bug_row, WebView::ui_text("Report a bug"sv));
    report_bug_text->addWidget(create_detail_label(WebView::ui_text("On GitHub"sv), *report_bug_row));
    m_external_link_icon = create_row_icon(*report_bug_row, EXTERNAL_LINK_ICON_SIZE);
    report_bug_row->layout()->addWidget(m_external_link_icon);
    QObject::connect(report_bug_row, &QPushButton::clicked, this, [this] {
        close();
        if (on_report_bug)
            on_report_bug();
    });
    layout->addWidget(report_bug_row);

    m_crash_reports_section = new QWidget(&card());
    auto* section_layout = new QVBoxLayout(m_crash_reports_section);
    section_layout->setContentsMargins(0, 5, 0, 0);
    section_layout->setSpacing(5);

    auto* separator = new QFrame(m_crash_reports_section);
    separator->setObjectName("LadybirdPopoverSeparator");
    separator->setFixedHeight(1);
    auto* separator_row = new QHBoxLayout;
    separator_row->setContentsMargins(10, 0, 10, 0);
    separator_row->addWidget(separator);
    section_layout->addLayout(separator_row);

    // NB: The Feedback button's dot sits right before the count, which keeps the row's title in line with the others.
    auto* crash_reports_row = create_row(*m_crash_reports_section, WebView::ui_text("Crash reports"sv), ROW_HEIGHT);
    create_row_text(*crash_reports_row, WebView::ui_text("Crash reports"sv));
    auto* count_layout = new QHBoxLayout;
    count_layout->setContentsMargins(0, 0, 0, 0);
    count_layout->setSpacing(CRASH_REPORTS_DOT_GAP);
    m_crash_reports_dot = create_row_icon(*crash_reports_row, CRASH_REPORTS_DOT_SIZE);
    count_layout->addWidget(m_crash_reports_dot);
    m_crash_reports_count = create_detail_label(QString {}, *crash_reports_row);
    count_layout->addWidget(m_crash_reports_count);
    static_cast<QHBoxLayout*>(crash_reports_row->layout())->addLayout(count_layout);
    m_crash_reports_chevron = create_row_icon(*crash_reports_row, CHEVRON_ICON_SIZE);
    crash_reports_row->layout()->addWidget(m_crash_reports_chevron);
    QObject::connect(crash_reports_row, &QPushButton::clicked, this, [this] {
        close();
        if (on_review_crash_reports)
            on_review_crash_reports();
    });
    section_layout->addWidget(crash_reports_row);
    layout->addWidget(m_crash_reports_section);
}

void FeedbackPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::feedback_popover_style_sheet(palette));

    auto const& panel = Theme::panel();
    auto device_pixel_ratio = devicePixelRatioF();
    m_external_link_icon->setPixmap(create_chrome_icon(ChromeIcon::ExternalLink, panel.muted).pixmap(QSize(EXTERNAL_LINK_ICON_SIZE, EXTERNAL_LINK_ICON_SIZE), device_pixel_ratio));
    m_crash_reports_chevron->setPixmap(create_chrome_icon(ChromeIcon::Forward, panel.muted).pixmap(QSize(CHEVRON_ICON_SIZE, CHEVRON_ICON_SIZE), device_pixel_ratio));

    QPixmap dot(QSize(CRASH_REPORTS_DOT_SIZE, CRASH_REPORTS_DOT_SIZE) * device_pixel_ratio);
    dot.setDevicePixelRatio(device_pixel_ratio);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ChromeStyle::crash_reports_dot_color(palette));
    painter.drawEllipse(QRectF(0, 0, CRASH_REPORTS_DOT_SIZE, CRASH_REPORTS_DOT_SIZE));
    painter.end();
    m_crash_reports_dot->setPixmap(dot);
}

void FeedbackPopover::set_reportable_host(QString const& host)
{
    m_host_label->setText(host);
    m_report_site_row->setVisible(!host.isEmpty());
}

void FeedbackPopover::set_crash_report_count(size_t count)
{
    bind_ui_text(*m_crash_reports_count, "count"sv, WebView::ui_text("{0} to review"sv, { String::number(count) }), [this](QString const& text) {
        m_crash_reports_count->setText(text);
    });
    m_crash_reports_section->setVisible(count > 0);
}

}
