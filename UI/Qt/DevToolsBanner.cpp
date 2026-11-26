/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/DevToolsBanner.h>
#include <UI/Qt/Localization.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace Ladybird {

DevToolsBanner::DevToolsBanner(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdDevToolsBanner");
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    update_chrome_style();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 3, 12, 3);
    layout->setSpacing(6);

    m_label = new QLabel(this);
    layout->addWidget(m_label);
    layout->addStretch();

    auto* launch_client_button = new QPushButton(this);
    bind_ui_text(*launch_client_button, WebView::ui_text("Open client"sv), [launch_client_button](QString const& text) {
        launch_client_button->setText(text);
    });
    connect(launch_client_button, &QPushButton::clicked, this, &DevToolsBanner::launch_client_requested);
    layout->addWidget(launch_client_button);

    auto* disable_button = new QPushButton(this);
    bind_ui_text(*disable_button, WebView::ui_text("Disable"sv), [disable_button](QString const& text) {
        disable_button->setText(text);
    });
    connect(disable_button, &QPushButton::clicked, this, &DevToolsBanner::disable_requested);
    layout->addWidget(disable_button);
}

void DevToolsBanner::set_port(u16 port)
{
    bind_ui_text(*m_label, "text"sv, WebView::ui_text("DevTools is enabled on port {0}"sv, { String::number(port) }), [this](QString const& text) {
        m_label->setText(text);
    });
}

bool DevToolsBanner::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();

    return QWidget::event(event);
}

void DevToolsBanner::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::devtools_banner_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
