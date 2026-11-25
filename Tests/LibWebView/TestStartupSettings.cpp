/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>

static ByteString settings_path()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-startup-settings-{}.json", Core::System::getpid())).string();
}

static void remove_settings_file()
{
    if (FileSystem::exists(settings_path()))
        MUST(FileSystem::remove(settings_path(), FileSystem::RecursionMode::Disallowed));
}

static void write_settings_file(StringView contents)
{
    auto file = MUST(Core::File::open(settings_path(), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
}

TEST_CASE(restoring_the_previous_session_is_off_by_default)
{
    remove_settings_file();

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.restore_previous_session());
}

TEST_CASE(restoring_the_previous_session_is_persistent)
{
    remove_settings_file();

    {
        auto settings = WebView::Settings::create(settings_path());
        settings.set_restore_previous_session(true);
    }

    {
        auto settings = WebView::Settings::create(settings_path());
        EXPECT(settings.restore_previous_session());
        settings.set_restore_previous_session(false);
    }

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.restore_previous_session());
    remove_settings_file();
}

TEST_CASE(a_non_boolean_saved_value_leaves_restoring_off)
{
    remove_settings_file();
    write_settings_file(R"({"restorePreviousSession":"yes"})"sv);

    auto settings = WebView::Settings::create(settings_path());
    EXPECT(!settings.restore_previous_session());
    remove_settings_file();
}

TEST_CASE(interface_language_is_persistent_and_rejects_unknown_values)
{
    remove_settings_file();
    {
        auto settings = WebView::Settings::create(settings_path());
        EXPECT_EQ(settings.config_variable(WebView::ConfigVariableID::InterfaceLanguage).as_string(), "en"sv);
        settings.set_config_variable(WebView::ConfigVariableID::InterfaceLanguage, "es"_string);
    }
    {
        auto settings = WebView::Settings::create(settings_path());
        EXPECT_EQ(settings.config_variable(WebView::ConfigVariableID::InterfaceLanguage).as_string(), "es"sv);
        settings.set_config_variable(WebView::ConfigVariableID::InterfaceLanguage, "invalid"_string);
        EXPECT_EQ(settings.config_variable(WebView::ConfigVariableID::InterfaceLanguage).as_string(), "es"sv);
        settings.set_config_variable(WebView::ConfigVariableID::InterfaceLanguage, "en"_string);
    }
    auto settings = WebView::Settings::create(settings_path());
    EXPECT_EQ(settings.config_variable(WebView::ConfigVariableID::InterfaceLanguage).as_string(), "en"sv);
    remove_settings_file();
}

TEST_CASE(invalid_interface_language_defaults_to_english_without_changing_website_languages)
{
    remove_settings_file();
    write_settings_file(R"({"languages":["es-CL","en-US"],"configVariables":{"ui.interface.language":"fr"}})"sv);

    auto settings = WebView::Settings::create(settings_path());
    EXPECT_EQ(settings.config_variable(WebView::ConfigVariableID::InterfaceLanguage).as_string(), "en"sv);
    auto website_languages = settings.languages();
    settings.set_config_variable(WebView::ConfigVariableID::InterfaceLanguage, "es"_string);
    EXPECT(settings.languages() == website_languages);
    remove_settings_file();
}
