/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Localization.h>

static constexpr auto SPANISH_CATALOG = R"({
    "": {
        "Reload": "Recargar",
        "{0} of {1}": "{1}: {0}",
        "Use {{braces}} for {0}": "Usa {{llaves}} para {0}",
        "{0} tab": { "one": "{0} pestaña", "other": "{0} pestañas" },
        "{0} window": { "one": "{0} ventana" },
        "Search": "Buscar",
        "Not text": 42,
        "Open {0}": "Abrir «{0}»…"
    },
    "settings-section": {
        "Search": "Búsqueda"
    },
    "not-an-object": "ignored"
})"sv;

static void use_spanish_test_catalog()
{
    TRY_OR_FAIL(WebView::load_catalog_for_testing("es"sv, SPANISH_CATALOG));
}

TEST_CASE(missing_entries_and_invalid_values_fall_back_to_the_source)
{
    use_spanish_test_catalog();
    EXPECT_EQ(WebView::translate(WebView::ui_text("Reload"sv)), "Recargar"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Not in the catalog"sv)), "Not in the catalog"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Not text"sv)), "Not text"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text_in("unknown-context"sv, "Reload"sv)), "Reload"sv);
    WebView::set_interface_language("en"sv);
}

TEST_CASE(unsupported_language_is_english)
{
    use_spanish_test_catalog();
    EXPECT(WebView::set_interface_language("fr"sv));
    EXPECT_EQ(WebView::interface_language(), "en"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Reload"sv)), "Reload"sv);
    EXPECT(!WebView::set_interface_language("invalid"sv));
    EXPECT(!WebView::set_interface_language("en"sv));
}

TEST_CASE(arguments_can_be_reordered_and_braces_escaped)
{
    use_spanish_test_catalog();
    EXPECT_EQ(WebView::translate(WebView::ui_text("{0} of {1}"sv, { "3"_string, "Downloads"_string })), "Downloads: 3"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Use {{braces}} for {0}"sv, { "{1}"_string })), "Usa {llaves} para {1}"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("{0} of {1}"sv, { "3"_string })), "{1}: 3"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Missing {0} and {9}, kept {"sv)), "Missing {0} and {9}, kept {"sv);
    WebView::set_interface_language("en"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("{0} of {1}"sv, { "3"_string, "Downloads"_string })), "3 of Downloads"sv);
}

TEST_CASE(plurals_follow_the_interface_language)
{
    TRY_OR_FAIL(WebView::load_catalog_for_testing("en"sv, "{}"sv));
    EXPECT_EQ(WebView::translate(WebView::ui_plural(0, "{0} tab"sv, "{0} tabs"sv)), "0 tabs"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_plural(1, "{0} tab"sv, "{0} tabs"sv)), "1 tab"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_plural(2, "{0} tab"sv, "{0} tabs"sv)), "2 tabs"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_plural(1000000, "{0} tab"sv, "{0} tabs"sv)), "1,000,000 tabs"sv);

    use_spanish_test_catalog();
    EXPECT_EQ(WebView::translate(WebView::ui_plural(0, "{0} tab"sv, "{0} tabs"sv)), "0 pestañas"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_plural(1, "{0} tab"sv, "{0} tabs"sv)), "1 pestaña"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_plural(2, "{0} tab"sv, "{0} tabs"sv)), "2 pestañas"sv);
    // Spanish has a "many" form for a million, which falls back to "other" when the catalog leaves it out.
    EXPECT_EQ(WebView::translate(WebView::ui_plural(1000000, "{0} tab"sv, "{0} tabs"sv)), "1.000.000 pestañas"sv);

    // Without an "other" form, a plural isn't a translation.
    EXPECT_EQ(WebView::translate(WebView::ui_plural(2, "{0} window"sv, "{0} windows"sv)), "2 windows"sv);
    WebView::set_interface_language("en"sv);
}

TEST_CASE(unicode_text_and_arguments_are_kept_whole)
{
    use_spanish_test_catalog();
    auto argument = MUST(String::from_utf8("año 🐞 {0}"sv));
    EXPECT_EQ(WebView::translate(WebView::ui_text("Open {0}"sv, { argument })), "Abrir «año 🐞 {0}»…"sv);
    WebView::set_interface_language("en"sv);
}

TEST_CASE(context_tells_identical_sources_apart)
{
    use_spanish_test_catalog();
    EXPECT_EQ(WebView::translate(WebView::ui_text("Search"sv)), "Buscar"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text_in("settings-section"sv, "Search"sv)), "Búsqueda"sv);
    WebView::set_interface_language("en"sv);
}

TEST_CASE(corrupt_catalog_leaves_english)
{
    EXPECT(WebView::load_catalog_for_testing("es"sv, R"({"": {"Reload": "Recargar")"sv).is_error());
    EXPECT_EQ(WebView::translate(WebView::ui_text("Reload"sv)), "Reload"sv);
    EXPECT(WebView::load_catalog_for_testing("es"sv, R"(["Reload", "Recargar"])"sv).is_error());
    EXPECT_EQ(WebView::translate(WebView::ui_text("Reload"sv)), "Reload"sv);
    WebView::set_interface_language("en"sv);
}

TEST_CASE(shipped_spanish_catalog_loads)
{
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    EXPECT(WebView::set_interface_language("es"sv));
    EXPECT_EQ(WebView::interface_language(), "es"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Settings"sv)), "Configuración"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("&File"sv)), "&Archivo"sv);
    // The settings page's text is the page's own, and never translates the same English on a browser control.
    EXPECT_EQ(WebView::translate(WebView::ui_text("Search"sv)), "Search"sv);
    EXPECT_EQ(WebView::translate(WebView::ui_text("Privacy and security"sv)), "Privacy and security"sv);
    EXPECT(WebView::set_interface_language("en"sv));
    EXPECT_EQ(WebView::translate(WebView::ui_text("Settings"sv)), "Settings"sv);
}
