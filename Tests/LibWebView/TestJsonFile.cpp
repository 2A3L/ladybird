/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/DirIterator.h>
#include <LibCore/File.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_LINUX)
#    include <signal.h>
#    include <sys/resource.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

static ByteString read_file(ByteString const& path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return ByteString::copy(MUST(file->read_until_eof()));
}

TEST_CASE(writes_and_replaces_json)
{
    Test::TemporaryDirectory directory { "TestJsonFile"sv };
    auto path = LexicalPath::join(directory.path().string(), "settings.json"sv).string();
    JsonObject first;
    first.set("value"sv, "old"sv);
    TRY_OR_FAIL(WebView::write_json_file(path, first));
    JsonObject replacement;
    replacement.set("value"sv, "new"sv);
    TRY_OR_FAIL(WebView::write_json_file(path, replacement));
    EXPECT_EQ(read_file(path), "{\"value\":\"new\"}"sv);
}

TEST_CASE(rejects_a_path_with_an_embedded_null)
{
    Test::TemporaryDirectory directory { "TestJsonFile"sv };
    auto path = LexicalPath::join(directory.path().string(), "settings.json"sv).string();
    JsonObject original;
    original.set("value"sv, "old"sv);
    TRY_OR_FAIL(WebView::write_json_file(path, original));
    auto malformed_path = ByteString::formatted("{}{}", path, StringView { "\0suffix", 7 });
    JsonObject replacement;
    replacement.set("value"sv, "new"sv);
    EXPECT(WebView::write_json_file(malformed_path, replacement).is_error());
    EXPECT_EQ(read_file(path), "{\"value\":\"old\"}"sv);
}

#if defined(AK_OS_LINUX)
TEST_CASE(failed_partial_write_preserves_original)
{
    Test::TemporaryDirectory directory { "TestJsonFile"sv };
    auto path = LexicalPath::join(directory.path().string(), "settings.json"sv).string();
    JsonObject original;
    original.set("value"sv, "old"sv);
    TRY_OR_FAIL(WebView::write_json_file(path, original));
    auto original_bytes = read_file(path);

    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        struct rlimit limit { 8, 8 };
        if (setrlimit(RLIMIT_FSIZE, &limit) < 0 || signal(SIGXFSZ, SIG_IGN) == SIG_ERR)
            _exit(2);
        JsonObject replacement;
        replacement.set("value"sv, "a much longer value that cannot fit in eight bytes"sv);
        _exit(WebView::write_json_file(path, replacement).is_error() ? 0 : 1);
    }
    int status;
    VERIFY(waitpid(pid, &status, 0) == pid);
    EXPECT(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(read_file(path), original_bytes);

    Core::DirIterator iterator(directory.path().string(), Core::DirIterator::SkipDots);
    EXPECT(!iterator.has_error());
    while (iterator.has_next())
        EXPECT_EQ(iterator.next_path(), "settings.json"sv);
    EXPECT(!iterator.has_error());
}

TEST_CASE(does_not_replace_a_symlink)
{
    Test::TemporaryDirectory directory { "TestJsonFile"sv };
    auto path = LexicalPath::join(directory.path().string(), "settings.json"sv).string();
    auto target = LexicalPath::join(directory.path().string(), "target.json"sv).string();
    JsonObject object;
    object.set("value"sv, "old"sv);
    TRY_OR_FAIL(WebView::write_json_file(target, object));
    VERIFY(symlink(target.characters(), path.characters()) == 0);
    JsonObject replacement;
    replacement.set("value"sv, "new"sv);
    EXPECT(WebView::write_json_file(path, replacement).is_error());
    struct stat link_stat {};
    VERIFY(lstat(path.characters(), &link_stat) == 0);
    EXPECT(S_ISLNK(link_stat.st_mode));
    EXPECT_EQ(read_file(target), "{\"value\":\"old\"}"sv);
}

TEST_CASE(replacement_has_no_group_or_other_access)
{
    Test::TemporaryDirectory directory { "TestJsonFile"sv };
    auto path = LexicalPath::join(directory.path().string(), "settings.json"sv).string();
    JsonObject object;
    object.set("value"sv, "old"sv);
    TRY_OR_FAIL(WebView::write_json_file(path, object));
    VERIFY(chmod(path.characters(), 0644) == 0);
    TRY_OR_FAIL(WebView::write_json_file(path, object));
    auto stat = TRY_OR_FAIL(Core::File::stat(path));
    EXPECT_EQ(stat.st_mode & 0077, 0);
}
#endif
