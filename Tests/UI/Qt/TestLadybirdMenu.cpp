/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/Localization.h>
#include <UI/Qt/Menu.h>

#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QWidget>

TEST_CASE(zoom_percentage_updates_while_menu_is_open)
{
    QMenu menu;
    QAction zoom_out(&menu);
    QAction reset_zoom(&menu);
    QAction zoom_in(&menu);
    auto confirmed_zoom = Ladybird::Action::create("100%"sv, Ladybird::ActionID::ResetZoomViaToolbar, [] { });
    confirmed_zoom->set_visible(false);
    Ladybird::ActionBinding zoom_level(menu, Ladybird::IncludeActionIcon::No);
    zoom_level.bind(*confirmed_zoom);
    auto* row = new Ladybird::MenuZoomRow(menu, zoom_out, reset_zoom, zoom_in, zoom_level.action());
    menu.addAction(row);
    auto* percentage = row->defaultWidget()->findChild<QPushButton*>("LadybirdMenuZoomLevel");
    VERIFY(percentage);

    QPushButton* zoom_in_button = nullptr;
    for (auto* button : row->defaultWidget()->findChildren<QPushButton*>("LadybirdMenuZoomButton")) {
        if (button->accessibleName() == QStringLiteral("Zoom In"))
            zoom_in_button = button;
    }
    int zoom_in_triggers = 0;
    QObject::connect(&zoom_in, &QAction::triggered, &menu, [&zoom_in_triggers] { ++zoom_in_triggers; });

    menu.popup({ 0, 0 });
    EXPECT(percentage->text() == QStringLiteral("100%"));

    VERIFY(zoom_in_button && zoom_in_button->isVisible());
    zoom_in_button->click();
    EXPECT_EQ(zoom_in_triggers, 1);
    EXPECT(percentage->text() == QStringLiteral("100%"));
    confirmed_zoom->set_text("110%"sv);
    confirmed_zoom->set_visible(true);
    EXPECT(percentage->text() == QStringLiteral("110%"));

    confirmed_zoom->set_text("120%"sv);
    EXPECT(percentage->text() == QStringLiteral("120%"));

    confirmed_zoom->set_visible(false);
    EXPECT(percentage->text() == QStringLiteral("100%"));
}

static void use_shipped_catalogs()
{
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
}

TEST_CASE(interface_text_is_translated_and_other_text_is_not)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("es"sv);
    QWidget parent;

    auto settings = Ladybird::Action::create(WebView::ui_text("Settings"sv), Ladybird::ActionID::OpenSettingsPage, [] { });
    Ladybird::ActionBinding settings_binding(parent, Ladybird::IncludeActionIcon::No);
    settings_binding.bind(*settings);
    EXPECT(settings_binding.action().text() == QStringLiteral("Configuración"));

    // A bookmark's title is the user's, even when it reads the same as text the catalog translates.
    auto bookmark = Ladybird::Action::create("Settings"_string, Ladybird::ActionID::BookmarkItem, [] { });
    Ladybird::ActionBinding bookmark_binding(parent, Ladybird::IncludeActionIcon::No);
    bookmark_binding.bind(*bookmark);
    EXPECT(bookmark_binding.action().text() == QStringLiteral("Settings"));

    auto literal = Ladybird::Action::create("Reload"sv, Ladybird::ActionID::Reload, [] { });
    EXPECT_EQ(literal->text(), "Reload"sv);

    settings->set_text("Custom title"sv);
    EXPECT(settings_binding.action().text() == QStringLiteral("Custom title"));

    Ladybird::apply_interface_language("unknown"sv);
    EXPECT(bookmark_binding.action().text() == QStringLiteral("Settings"));
    EXPECT_EQ(WebView::interface_language(), "en"sv);
}

TEST_CASE(rebinding_a_slot_keeps_only_the_latest_text)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("en"sv);
    QObject owner;
    QString shown;

    for (auto source : { "Reload"sv, "Back"sv, "Forward"sv, "Settings"sv })
        Ladybird::bind_ui_text(owner, "label"sv, WebView::ui_text(source), [&shown](QString const& text) { shown = text; });
    EXPECT(shown == QStringLiteral("Settings"));
    EXPECT_EQ(owner.children().size(), 1);

    Ladybird::apply_interface_language("es"sv);
    EXPECT(shown == QStringLiteral("Configuración"));
    EXPECT_EQ(owner.children().size(), 1);

    Ladybird::apply_interface_language("en"sv);
    EXPECT(shown == QStringLiteral("Settings"));
}

TEST_CASE(interface_language_changes_while_running)
{
    use_shipped_catalogs();
    Ladybird::apply_interface_language("es"sv);
    QWidget parent;

    auto reload = Ladybird::Action::create(WebView::ui_text("Reload"sv), Ladybird::ActionID::Reload, [] { });
    reload->set_tooltip(WebView::ui_text("Reload Without Cache"sv));
    auto menu = Ladybird::Menu::create(WebView::ui_text("Bookmarks"sv));
    menu->add_action(reload);
    auto* qmenu = Ladybird::create_application_menu(parent, *menu);
    VERIFY(qmenu && qmenu->actions().size() == 1);
    auto* qaction = qmenu->actions().first();

    QMenu help_menu;
    Ladybird::bind_ui_text(help_menu, WebView::ui_text("Help"sv), [&help_menu](QString const& title) { help_menu.setTitle(title); });

    QMenu zoom_menu;
    QAction zoom_out(&zoom_menu);
    QAction reset_zoom(&zoom_menu);
    QAction zoom_in(&zoom_menu);
    QAction zoom_level(&zoom_menu);
    auto* row = new Ladybird::MenuZoomRow(zoom_menu, zoom_out, reset_zoom, zoom_in, zoom_level);
    zoom_menu.addAction(row);
    auto* level = row->defaultWidget()->findChild<QPushButton*>("LadybirdMenuZoomLevel");
    VERIFY(level);

    auto expect_language = [&](QString const& reload_text, QString const& tooltip, QString const& menu_title, QString const& help_title, QString const& reset_zoom_text) {
        EXPECT(qaction->text() == reload_text);
        EXPECT(qaction->toolTip() == tooltip);
        EXPECT(qmenu->title() == menu_title);
        EXPECT(help_menu.title() == help_title);
        EXPECT(level->accessibleName() == reset_zoom_text);
    };

    expect_language(QStringLiteral("Recargar"), QStringLiteral("Recargar sin caché"), QStringLiteral("Marcadores"), QStringLiteral("Ayuda"), QStringLiteral("Restablecer zoom"));
    Ladybird::apply_interface_language("en"sv);
    expect_language(QStringLiteral("Reload"), QStringLiteral("Reload Without Cache"), QStringLiteral("Bookmarks"), QStringLiteral("Help"), QStringLiteral("Reset Zoom"));
    Ladybird::apply_interface_language("es"sv);
    expect_language(QStringLiteral("Recargar"), QStringLiteral("Recargar sin caché"), QStringLiteral("Marcadores"), QStringLiteral("Ayuda"), QStringLiteral("Restablecer zoom"));
    Ladybird::apply_interface_language("en"sv);
}
