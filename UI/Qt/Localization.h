/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/StringView.h>
#include <LibWebView/Localization.h>

#include <QString>

class QObject;

namespace Ladybird {

QString translate_to_qstring(WebView::UIText const&);

// Shows the text with the given function now, and again whenever the interface language changes, for as long as the
// owner lives. Each call adds a binding, so this is for text that is bound once, such as a fixed label.
void bind_ui_text(QObject& owner, WebView::UIText, Function<void(QString const&)> apply);

// The same for text that changes, such as a count: binding a slot again replaces what the owner showed in that slot
// before, so the owner keeps a single binding per slot, showing the latest text.
// NB: A slot must not be bound again from its own apply function, which binding again replaces while it runs.
void bind_ui_text(QObject& owner, StringView slot, WebView::UIText, Function<void(QString const&)> apply);

// Sets the interface language, and shows every action, menu and bound text in it.
void apply_interface_language(StringView language);

}
