/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/DownloadPresentation.h>
#include <UI/Qt/Localization.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/NotificationBar.h>

#include <QFontMetrics>
#include <QToolButton>
#include <QWidget>

namespace {

class EmptySuggestionProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual void query(WebView::AutocompleteQueryID query_id, String, size_t) override
    {
        on_suggestions(query_id, {}, WebView::AutocompleteResultKind::Final);
    }

    virtual void cancel() override { }
};

void use_shipped_catalogs()
{
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
}

}

TEST_CASE(location_edit_placeholder_follows_the_interface_language)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("en"sv);

    auto settings = WebView::Settings::create_from_json(JsonObject {});
    QWidget window;
    Ladybird::LocationEdit field(&window, settings, make<EmptySuggestionProvider>());
    EXPECT(field.placeholderText() == QStringLiteral("Search or enter address"));

    Ladybird::apply_interface_language("es"sv);
    EXPECT(field.placeholderText() == QStringLiteral("Buscar o escribir dirección"));

    Ladybird::apply_interface_language("en"sv);
    EXPECT(field.placeholderText() == QStringLiteral("Search or enter address"));
}

TEST_CASE(notification_bar_close_button_follows_the_interface_language)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("en"sv);

    QWidget window;
    Vector<Ladybird::NotificationBar::Button> buttons;
    Ladybird::NotificationBar bar(window, QStringLiteral("A message from a page"), AK::move(buttons), [] { });
    auto* close_button = bar.findChild<QToolButton*>();
    VERIFY(close_button);
    EXPECT(close_button->toolTip() == QStringLiteral("Dismiss"));

    Ladybird::apply_interface_language("es"sv);
    EXPECT(close_button->toolTip() == QStringLiteral("Descartar"));
    EXPECT(close_button->accessibleName() == QStringLiteral("Descartar"));

    Ladybird::apply_interface_language("en"sv);
    EXPECT(close_button->toolTip() == QStringLiteral("Dismiss"));
}

// The find in page bar is 430 pixels wide, and its search field is what is left of that beside four 28 pixel buttons,
// a separator, the margins and the gaps between them, about 260 pixels. The bar itself needs a tab and a web view, so
// this measures its texts with the default font instead: the placeholder, which shows while the field is empty, and
// the widest text of the result label, which shows beside the typed text. Each has to fit the field on its own; how
// much room is left for the typed text isn't asserted.
TEST_CASE(find_in_page_texts_fit_the_search_field)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("es"sv);

    QWidget widget;
    auto const& metrics = widget.fontMetrics();
    auto placeholder = Ladybird::translate_to_qstring(WebView::ui_text("Find in page"sv));
    auto count = Ladybird::translate_to_qstring(WebView::ui_text("{0} of {1}"sv, { "9999"_string, "9999"_string }));
    auto no_matches = Ladybird::translate_to_qstring(WebView::ui_text("No matches"sv));
    EXPECT(count == QStringLiteral("9999 de 9999"));
    EXPECT(no_matches == QStringLiteral("Sin coincidencias"));

    // The field's own icon, margins and gaps take about 60 pixels of it.
    constexpr int field_text_width = 260 - 60;
    auto result_width = max(metrics.horizontalAdvance(count), metrics.horizontalAdvance(no_matches));
    EXPECT(metrics.horizontalAdvance(placeholder) <= field_text_width);
    EXPECT(result_width <= field_text_width);

    Ladybird::apply_interface_language("en"sv);
}

// A text bound to a slot is replaced by the next one bound to it, and the language shows the latest.
TEST_CASE(slot_bound_text_follows_the_state_and_the_language)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("en"sv);

    QWidget button;
    auto show = [&](StringView text) {
        Ladybird::bind_ui_text(button, "tooltip"sv, WebView::ui_text(text), [&button](QString const& text) {
            button.setToolTip(text);
        });
    };
    show("Pause"sv);
    EXPECT(button.toolTip() == QStringLiteral("Pause"));
    show("Resume"sv);
    EXPECT(button.toolTip() == QStringLiteral("Resume"));

    Ladybird::apply_interface_language("es"sv);
    EXPECT(button.toolTip() == QStringLiteral("Reanudar"));
    show("Pause"sv);
    EXPECT(button.toolTip() == QStringLiteral("Pausar"));

    Ladybird::apply_interface_language("en"sv);
    EXPECT(button.toolTip() == QStringLiteral("Pause"));
}

TEST_CASE(tab_search_placeholder_is_a_plural)
{
    use_shipped_catalogs();
    auto placeholder = [](i64 count) {
        return Ladybird::translate_to_qstring(WebView::ui_plural(count, "Search {0} tab"sv, "Search {0} tabs"sv));
    };

    Ladybird::apply_interface_language("en"sv);
    EXPECT(placeholder(1) == QStringLiteral("Search 1 tab"));
    EXPECT(placeholder(0) == QStringLiteral("Search 0 tabs"));
    EXPECT(placeholder(7) == QStringLiteral("Search 7 tabs"));

    Ladybird::apply_interface_language("es"sv);
    EXPECT(placeholder(1) == QStringLiteral("Buscar en 1 pestaña"));
    EXPECT(placeholder(7) == QStringLiteral("Buscar en 7 pestañas"));

    Ladybird::apply_interface_language("en"sv);
}

TEST_CASE(download_times_follow_the_interface_language)
{
    use_shipped_catalogs();
    auto text = [](i64 seconds) {
        return Ladybird::download_time_remaining_text(AK::Duration::from_seconds(seconds));
    };

    Ladybird::apply_interface_language("en"sv);
    EXPECT_EQ(text(61 * 60), "1 hour 1 minute left"_string);

    Ladybird::apply_interface_language("es"sv);
    EXPECT_EQ(text(0), "falta 1 segundo"_string);
    EXPECT_EQ(text(45), "faltan 45 segundos"_string);
    EXPECT_EQ(text(60), "falta 1 minuto"_string);
    EXPECT_EQ(text(7199), "faltan 2 horas"_string);
    EXPECT_EQ(text(61 * 60), "faltan 1 hora y 1 minuto"_string);

    Ladybird::apply_interface_language("en"sv);
}
