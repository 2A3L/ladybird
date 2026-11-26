/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NeverDestroyed.h>
#include <AK/OwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibCore/Resource.h>
#include <LibUnicode/NumberFormat.h>
#include <LibWebView/Localization.h>

namespace WebView {

// Each part of the interface has its own file, so translations of one part can change without touching the others. Each
// file is also a scope: the browser controls read only their own, so text of the browser's pages, such as the
// settings page's "Search" section, can never translate the same English on a control. The pages' own files, settings
// and pages, are read only by the pages.
static constexpr Array CATALOG_SURFACES = { "chrome"sv, "actions"sv, "widgets"sv, "qt"sv };

struct Catalog {
    String language { "en"_string };
    // Keyed by the context and the source text, apart by a character that neither contains.
    HashMap<String, JsonValue> entries;
    OwnPtr<Unicode::NumberFormat> number_format;
};

static Catalog& catalog()
{
    static NeverDestroyed<Catalog> catalog;
    return *catalog;
}

static String entry_key(StringView context, StringView source)
{
    return MUST(String::formatted("{}\x04{}", context, source));
}

// The catalog is data from outside the program, so whatever in it isn't a translation is left out rather than trusted.
static ErrorOr<void> add_entries(JsonValue const& json)
{
    if (!json.is_object())
        return Error::from_string_literal("A catalog must be an object of contexts");

    auto& entries = catalog().entries;
    json.as_object().for_each_member([&](String const& context, JsonValue const& messages) {
        if (!messages.is_object()) {
            warnln("Ignoring the interface text context '{}', which is not an object", context);
            return;
        }
        messages.as_object().for_each_member([&](String const& source, JsonValue const& translation) {
            entries.set(entry_key(context, source), translation);
        });
    });
    return {};
}

static ErrorOr<void> load_surface(StringView language, StringView surface)
{
    auto uri = TRY(String::formatted("resource://ladybird/locale/{}/{}.json", language, surface));
    auto resource = TRY(Core::Resource::load_from_uri(uri));
    auto json = TRY(JsonValue::from_string(StringView { resource->data() }));
    return add_entries(json);
}

static void load_catalog(StringView language)
{
    for (auto surface : CATALOG_SURFACES) {
        if (auto result = load_surface(language, surface); result.is_error())
            warnln("Cannot load the {} interface text for '{}', it stays in English: {}", surface, language, result.error());
    }
}

static void reset_catalog(StringView language)
{
    auto& catalog = WebView::catalog();
    catalog.language = MUST(String::from_utf8(language));
    catalog.entries.clear();
    catalog.number_format = nullptr;
}

static Unicode::NumberFormat& number_format()
{
    auto& catalog = WebView::catalog();
    if (!catalog.number_format) {
        Unicode::RoundingOptions rounding_options;
        rounding_options.type = Unicode::RoundingType::FractionDigits;
        rounding_options.min_fraction_digits = 0;
        rounding_options.max_fraction_digits = 0;

        auto locale = Utf16String::from_utf8(catalog.language);
        catalog.number_format = Unicode::NumberFormat::create(locale.utf16_view(), {}, rounding_options);
        catalog.number_format->create_plural_rules(Unicode::PluralForm::Cardinal);
    }
    return *catalog.number_format;
}

static String plural_category(i64 count)
{
    auto category = number_format().select_plural(static_cast<double>(count));
    return MUST(Unicode::plural_category_to_string(category).to_utf8());
}

// Only ASCII is matched, so the text between placeholders stays whole UTF-8.
static String substitute_arguments(StringView pattern, ReadonlySpan<String> arguments)
{
    StringBuilder builder;
    for (size_t i = 0; i < pattern.length(); ++i) {
        auto character = pattern[i];
        auto next = i + 1 < pattern.length() ? pattern[i + 1] : '\0';

        if ((character == '{' || character == '}') && next == character) {
            builder.append(character);
            ++i;
            continue;
        }
        if (character == '{' && is_ascii_digit(next) && i + 2 < pattern.length() && pattern[i + 2] == '}') {
            auto index = parse_ascii_digit(next);
            if (index < arguments.size())
                builder.append(arguments[index]);
            else
                builder.append(pattern.substring_view(i, 3));
            i += 2;
            continue;
        }
        builder.append(character);
    }
    return MUST(builder.to_string());
}

UIText ui_text(StringView source, Vector<String> arguments)
{
    return { .source = source, .context = {}, .arguments = move(arguments), .count = {}, .plural_source = {} };
}

UIText ui_text_in(StringView context, StringView source, Vector<String> arguments)
{
    return { .source = source, .context = context, .arguments = move(arguments), .count = {}, .plural_source = {} };
}

UIText ui_plural(i64 count, StringView one, StringView other, Vector<String> arguments)
{
    return { .source = one, .context = {}, .arguments = move(arguments), .count = count, .plural_source = other };
}

Optional<String> find_translation(StringView context, StringView source, Optional<i64> count)
{
    auto const& entries = catalog().entries;
    if (entries.is_empty())
        return {};

    auto entry = entries.get(entry_key(context, source));
    if (!entry.has_value())
        return {};
    if (entry->is_string())
        return entry->as_string();
    if (!entry->is_object())
        return {};

    // A form the language has, but the catalog doesn't, is the "other" form, as is every form of a text without a count.
    auto const& forms = entry->as_object();
    if (count.has_value()) {
        if (auto form = forms.get_string(plural_category(*count)); form.has_value())
            return *form;
    }
    if (auto form = forms.get_string("other"sv); form.has_value())
        return *form;
    return {};
}

String translate(UIText const& text)
{
    Vector<String> arguments;
    if (text.count.has_value())
        arguments.append(number_format().format(static_cast<double>(*text.count)).to_utf8());
    arguments.extend(text.arguments);

    if (auto translation = find_translation(text.context, text.source, text.count); translation.has_value())
        return substitute_arguments(*translation, arguments);

    auto source = text.source;
    if (text.count.has_value() && *text.count != 1 && !text.plural_source.is_empty())
        source = text.plural_source;
    return substitute_arguments(source, arguments);
}

bool set_interface_language(StringView language)
{
    auto supported_language = language == "es"sv ? "es"sv : "en"sv;
    if (catalog().language == supported_language)
        return false;

    reset_catalog(supported_language);
    if (supported_language != "en"sv)
        load_catalog(supported_language);
    return true;
}

StringView interface_language()
{
    return catalog().language;
}

ErrorOr<void> load_catalog_for_testing(StringView language, StringView json)
{
    reset_catalog(language);
    return add_entries(TRY(JsonValue::from_string(json)));
}

}
