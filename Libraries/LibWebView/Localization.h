/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// A piece of browser interface text. It is identified by its English source text and, where the same English has
// different meanings, a context. Arguments replace the positional placeholders {0} to {9}; {{ and }} are literal
// braces. A plural's count is argument {0}, formatted for the interface language, and its catalog entry is found by
// its "one" form. There is no plural with a context.
//
// NB: The source, context and plural source are views, which are kept as long as the text is shown and translated
//     again when the language changes. They must be string literals, or other text that lives as long as the program.
//
// The catalog belongs to the thread of the browser controls, and is neither read nor changed on any other thread.
// See Documentation/InterfaceTranslations.md for where text goes.
struct UIText {
    StringView source;
    StringView context;
    Vector<String> arguments;
    Optional<i64> count;
    StringView plural_source;
};

WEBVIEW_API UIText ui_text(StringView source, Vector<String> arguments = {});
WEBVIEW_API UIText ui_text_in(StringView context, StringView source, Vector<String> arguments = {});
WEBVIEW_API UIText ui_plural(i64 count, StringView one, StringView other, Vector<String> arguments = {});

// The text in the interface language, or in English where the catalog has no translation for it.
WEBVIEW_API String translate(UIText const&);

// The catalog's translation of the source text as it is, with its placeholders left in place.
WEBVIEW_API Optional<String> find_translation(StringView context, StringView source, Optional<i64> count = {});

// Any language other than a supported one is English. Returns whether the language changed. Only the catalog files of
// the browser controls are read; those of the browser's pages are read by the pages.
WEBVIEW_API bool set_interface_language(StringView language);
WEBVIEW_API StringView interface_language();

WEBVIEW_API ErrorOr<void> load_catalog_for_testing(StringView language, StringView json);

}
