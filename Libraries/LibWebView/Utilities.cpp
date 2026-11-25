/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf16String.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

#if defined(AK_OS_LINUX)
#    include <linux/magic.h>
#endif

#if defined(AK_OS_LINUX)
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace WebView {

ErrorOr<Web::HTML::SelectedFile> create_selected_file(ByteString const& file_path)
{
    // FIXME: Implement the File and Directory Entries API.
    //        https://wicg.github.io/entries-api/
    if (FileSystem::is_directory(file_path))
        return Error::from_string_literal("Only files may currently be selected");

    // https://html.spec.whatwg.org/multipage/input.html#file-upload-state-(type=file):concept-input-file-path
    // Filenames must not contain path components, even in the case that a user has selected an entire directory
    // hierarchy or multiple files with the same name from different directories.
    auto name = Utf16String::from_utf8(LexicalPath::basename(file_path));

    auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
    return Web::HTML::SelectedFile { move(name), IPC::File::adopt_file(move(file)) };
}

static ErrorOr<OpenedLocalFile> open_local_file(ByteString const& path, Optional<ReadonlySpan<LocalFileRoot>> roots)
{
#if defined(AK_OS_WINDOWS)
    // FIXME: Check the roots here too, once FileSystem::real_path() resolves links and drive-letter paths.
    (void)roots;
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));

    // Files and directories are disk handles, while devices and pipes are not.
    if (GetFileType(to_handle(file->fd())) != FILE_TYPE_DISK)
        return Error::from_errno(EACCES);
    return OpenedLocalFile { IPC::File::adopt_file(move(file)), LexicalPath::canonicalized_path(path) };
#else
    auto canonical_path = TRY(FileSystem::real_path(path));
    if (roots.has_value() && !any_of(*roots, [&](auto const& root) { return root.covers(canonical_path); }))
        return Error::from_errno(EACCES);

#    if defined(AK_OS_MACOS)
    // A /dev/fd entry opens a descriptor of the UI process, which reads as whatever file is behind it. Only the path
    // says it's on devfs, so this is checked before the open.
    auto path_file_system = TRY(Core::System::statfs(canonical_path));
    if (StringView { path_file_system.f_fstypename, strnlen(path_file_system.f_fstypename, sizeof(path_file_system.f_fstypename)) } == "devfs"sv)
        return Error::from_errno(EACCES);
#    endif

    auto file = TRY(Core::File::open_regular_file_or_directory(canonical_path));

#    if defined(AK_OS_LINUX)
    // procfs and sysfs can be mounted anywhere and expose information about the opening process.
    auto file_system = TRY(Core::System::fstatfs(file->fd()));
    if (file_system.f_type == PROC_SUPER_MAGIC || file_system.f_type == SYSFS_MAGIC)
        return Error::from_errno(EACCES);
#    endif

    return OpenedLocalFile { IPC::File::adopt_file(move(file)), move(canonical_path) };
#endif
}

ErrorOr<OpenedLocalFile> open_local_file_for_renderer(ByteString const& path, ReadonlySpan<LocalFileRoot> roots)
{
    // NB: Windows doesn't check the roots yet, so a renderer without any is refused before it gets there.
    if (roots.is_empty())
        return Error::from_errno(EACCES);
    return open_local_file(path, roots);
}

ErrorOr<OpenedLocalFile> open_local_file_the_user_chose(ByteString const& path)
{
    return open_local_file(path, {});
}

ErrorOr<JsonObject> read_json_file(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error()) {
        if (file.error().is_errno() && file.error().code() == ENOENT)
            return JsonObject {};
        return file.release_error();
    }

    auto contents = TRY(file.value()->read_until_eof());
    auto json = TRY(JsonValue::from_string(contents));

    if (!json.is_object())
        return Error::from_string_literal("Expected parsed JSON value to be an object");
    return move(json.as_object());
}

ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value)
{
    if (path.contains('\0'))
        return Error::from_errno(EINVAL);
    auto directory = LexicalPath { path }.parent();
    auto parent = TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));

#if !defined(AK_OS_LINUX)
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(value.serialized()));
#else
    auto filename = LexicalPath::basename(path);
    auto existing = Core::System::fstatat(parent.fd(), filename, AT_SYMLINK_NOFOLLOW);
    if (existing.is_error()) {
        if (!existing.error().is_errno() || existing.error().code() != ENOENT)
            return existing.release_error();
    } else {
        if (!S_ISREG(existing.value().st_mode))
            return Error::from_errno(EINVAL);
        if (existing.value().st_uid != geteuid())
            return Error::from_errno(EACCES);
        // Check write access without truncating the original or following a replaced link.
        auto original = TRY(parent.open(filename, Core::File::OpenMode::Write | Core::File::OpenMode::Append | Core::File::OpenMode::DontCreate | Core::File::OpenMode::NoFollow | Core::File::OpenMode::Nonblocking));
        auto opened = TRY(original->stat());
        if (!S_ISREG(opened.st_mode) || opened.st_dev != existing.value().st_dev || opened.st_ino != existing.value().st_ino)
            return Error::from_errno(EAGAIN);
    }

    auto temporary_name = ByteString::formatted(".ladybird-json-{}.tmp", generate_random_uuid());
    // Restrict the Linux temporary file to the owner, including inherited POSIX ACL entries.
    auto file = TRY(parent.open(temporary_name, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew | Core::File::OpenMode::NoFollow, 0600));
    ArmedScopeGuard remove_temporary = [&] { (void)unlinkat(parent.fd(), temporary_name.characters(), 0); };
    auto fd = file->leak_fd();
    ArmedScopeGuard close_temporary = [&] { (void)Core::System::close(fd); };
    TRY(file->write_until_depleted(value.serialized()));
    if (fsync(fd) < 0)
        return Error::from_errno(errno);
    auto close_result = Core::System::close(fd);
    close_temporary.disarm();
    TRY(close_result);
    TRY(Core::System::renameat(parent.fd(), temporary_name, parent.fd(), filename));
    remove_temporary.disarm();
    // Publication has completed; a sync failure affects durability, not the caller's saved state.
    if (fsync(parent.fd()) < 0)
        warnln("Unable to sync JSON file directory: {}", Error::from_errno(errno));
#endif

    return {};
}

}
