/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibCore/File.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CacheStorageStore.h>

using WebView::CacheStorageStore;

#if defined(AK_OS_LINUX)
#    include <sys/stat.h>
#    include <unistd.h>
#endif

TEST_CASE(open_names_are_ordered_and_partitioned)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto first = TRY_OR_FAIL(store->open("partition-a"_string, "first"_utf16));
    auto second = TRY_OR_FAIL(store->open("partition-a"_string, "second"_utf16));
    auto again = TRY_OR_FAIL(store->open("partition-a"_string, "first"_utf16));
    auto other = TRY_OR_FAIL(store->open("partition-b"_string, "first"_utf16));
    EXPECT_EQ(first, again);
    EXPECT_NE(first, other);
    EXPECT_NE(first, second);
    auto names = TRY_OR_FAIL(store->list("partition-a"_string));
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "first"_utf16);
    EXPECT_EQ(names[1], "second"_utf16);
    EXPECT(store->read("partition-b"_string, first).is_error());
}

TEST_CASE(separate_stores_do_not_share_names_or_snapshots)
{
    auto first = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto second = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto key = "partition"_string;
    auto id = TRY_OR_FAIL(first->open(key, "name"_utf16));
    TRY_OR_FAIL(first->commit(key, id, 0, "private"sv.bytes()));
    EXPECT(!TRY_OR_FAIL(second->has(key, "name"_utf16)));
    EXPECT(TRY_OR_FAIL(second->list(key)).is_empty());
    auto other_id = TRY_OR_FAIL(second->open(key, "name"_utf16));
    EXPECT(TRY_OR_FAIL(second->read(key, other_id)).bytes.is_empty());
    auto snapshot = TRY_OR_FAIL(first->read(key, id));
    EXPECT_EQ(StringView { snapshot.bytes }, "private"sv);
}

TEST_CASE(deleted_cache_remains_available_to_its_leases)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto key = "partition"_string;
    auto old_id = TRY_OR_FAIL(store->open(key, "name"_utf16));
    EXPECT_EQ(TRY_OR_FAIL(store->commit(key, old_id, 0, "old"sv.bytes())), 1u);
    EXPECT(TRY_OR_FAIL(store->remove(key, "name"_utf16)));
    EXPECT(!TRY_OR_FAIL(store->remove(key, "name"_utf16)));
    EXPECT(TRY_OR_FAIL(store->list(key)).is_empty());
    auto new_id = TRY_OR_FAIL(store->open(key, "name"_utf16));
    EXPECT_NE(old_id, new_id);
    auto old_snapshot = TRY_OR_FAIL(store->read(key, old_id));
    EXPECT_EQ(StringView { old_snapshot.bytes }, "old"sv);
    EXPECT(TRY_OR_FAIL(store->read(key, new_id)).bytes.is_empty());
    TRY_OR_FAIL(store->release(key, old_id));
    EXPECT(store->read(key, old_id).is_error());
    TRY_OR_FAIL(store->release(key, new_id));
    EXPECT(store->read(key, new_id).is_error());
    auto found = TRY_OR_FAIL(store->find(key, "name"_utf16));
    EXPECT(found.has_value());
    EXPECT_EQ(*found, new_id);
    EXPECT(TRY_OR_FAIL(store->read(key, new_id)).bytes.is_empty());
}

TEST_CASE(stale_commit_does_not_change_snapshot)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto key = "partition"_string;
    auto id = TRY_OR_FAIL(store->open(key, "name"_utf16));
    EXPECT_EQ(TRY_OR_FAIL(store->commit(key, id, 0, "first"sv.bytes())), 1u);
    auto empty = store->commit(key, id, 1, {});
    EXPECT(empty.is_error());
    EXPECT_EQ(empty.error().code(), EINVAL);
    auto stale = store->commit(key, id, 0, "second"sv.bytes());
    EXPECT(stale.is_error());
    EXPECT(stale.error().is_errno());
    EXPECT_EQ(stale.error().code(), EAGAIN);
    auto snapshot = TRY_OR_FAIL(store->read(key, id));
    EXPECT_EQ(snapshot.revision, 1u);
    EXPECT_EQ(StringView { snapshot.bytes }, "first"sv);
}

TEST_CASE(name_round_trips_null_and_lonely_surrogate)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    char16_t const units[] { 'a', 0, 0xd800 };
    char16_t const replacement_units[] { 'a', 0, 0xfffd };
    auto name = Utf16String::from_utf16(Utf16View { units, 3 });
    auto replacement_name = Utf16String::from_utf16(Utf16View { replacement_units, 3 });
    auto id = TRY_OR_FAIL(store->open("partition"_string, name));
    auto replacement = TRY_OR_FAIL(store->open("partition"_string, replacement_name));
    EXPECT_NE(id, replacement);
    auto names = TRY_OR_FAIL(store->list("partition"_string));
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], name);
    EXPECT_EQ(names[1], replacement_name);
    EXPECT_EQ(*TRY_OR_FAIL(store->find("partition"_string, name)), id);
}

TEST_CASE(quotas_include_unlinked_caches_until_their_last_lease)
{
    CacheStorageStore::Limits limits;
    limits.snapshot_bytes = 4;
    limits.partition_bytes = 270;
    limits.session_bytes = 518;
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed(limits));
    auto first = TRY_OR_FAIL(store->open("a"_string, "x"_utf16));
    auto second = TRY_OR_FAIL(store->open("b"_string, "x"_utf16));
    EXPECT(store->open("a"_string, "y"_utf16).is_error());
    EXPECT(store->commit("b"_string, second, 0, "12345"sv.bytes()).is_error());
    EXPECT(store->commit("b"_string, second, 0, "1234"sv.bytes()).is_error());
    EXPECT(TRY_OR_FAIL(store->remove("a"_string, "x"_utf16)));
    EXPECT(store->commit("b"_string, second, 0, "1234"sv.bytes()).is_error());
    TRY_OR_FAIL(store->release("a"_string, first));
    EXPECT_EQ(TRY_OR_FAIL(store->commit("b"_string, second, 0, "1234"sv.bytes())), 1u);
}

TEST_CASE(identity_name_and_lease_limits_are_bounded)
{
    CacheStorageStore::Limits limits;
    limits.names_per_partition = 1;
    limits.name_code_units = 1;
    limits.identities = 2;
    limits.leases = 2;
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed(limits));
    auto first = TRY_OR_FAIL(store->open("a"_string, "x"_utf16));
    EXPECT(store->open("a"_string, "xy"_utf16).is_error());
    EXPECT(store->open("a"_string, "y"_utf16).is_error());
    auto second_lease = TRY_OR_FAIL(store->open("a"_string, "x"_utf16));
    EXPECT_EQ(first, second_lease);
    EXPECT(store->open("b"_string, "x"_utf16).is_error());
    TRY_OR_FAIL(store->release("a"_string, first));
    auto second = TRY_OR_FAIL(store->open("b"_string, "x"_utf16));
    TRY_OR_FAIL(store->release("a"_string, first));
    TRY_OR_FAIL(store->release("b"_string, second));
    EXPECT(store->open("c"_string, "x"_utf16).is_error());
}

TEST_CASE(enumeration_acquires_all_leases_or_none)
{
    CacheStorageStore::Limits limits;
    limits.leases = 3;
    limits.leases_per_partition = 3;
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed(limits));
    auto key = "partition"_string;
    auto first = TRY_OR_FAIL(store->open(key, "first"_utf16));
    auto second = TRY_OR_FAIL(store->open(key, "second"_utf16));
    EXPECT(TRY_OR_FAIL(store->has(key, "first"_utf16)));
    EXPECT(!TRY_OR_FAIL(store->has(key, "missing"_utf16)));
    auto refused = store->enumerate(key);
    EXPECT(refused.is_error());
    EXPECT_EQ(refused.error().code(), ENOSPC);
    TRY_OR_FAIL(store->release(key, first));
    TRY_OR_FAIL(store->release(key, second));
    EXPECT(store->read(key, first).is_error());
    EXPECT(store->read(key, second).is_error());
    EXPECT(TRY_OR_FAIL(store->has(key, "first"_utf16)));
    EXPECT(store->read(key, first).is_error());
    auto other_key = "other"_string;
    auto other = TRY_OR_FAIL(store->open(other_key, "first"_utf16));
    TRY_OR_FAIL(store->release(other_key, other));
    auto detached = TRY_OR_FAIL(store->open(key, "detached"_utf16));
    EXPECT(TRY_OR_FAIL(store->remove(key, "detached"_utf16)));

    auto ids = TRY_OR_FAIL(store->enumerate(key));
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], first);
    EXPECT_EQ(ids[1], second);
    TRY_OR_FAIL(store->read(key, first));
    TRY_OR_FAIL(store->read(key, second));
    auto refused_find = store->find(key, "first"_utf16);
    EXPECT(refused_find.is_error());
    EXPECT_EQ(refused_find.error().code(), ENOSPC);
    EXPECT(!TRY_OR_FAIL(store->find(key, "missing"_utf16)).has_value());
    TRY_OR_FAIL(store->release(key, first));
    TRY_OR_FAIL(store->release(key, second));
    EXPECT(store->read(other_key, other).is_error());
    TRY_OR_FAIL(store->release(key, detached));
    EXPECT(store->read(key, detached).is_error());
}

TEST_CASE(clear_since_invalidates_only_recent_visible_and_detached_caches)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto time = UnixDateTime::from_nanoseconds_since_epoch(1'000);
    store->set_clock_for_testing([&] { return time; });
    auto key = "partition"_string;
    auto older = TRY_OR_FAIL(store->open(key, "older"_utf16));
    auto older_detached = TRY_OR_FAIL(store->open(key, "older-detached"_utf16));
    EXPECT(TRY_OR_FAIL(store->remove(key, "older-detached"_utf16)));
    auto recently_detached = TRY_OR_FAIL(store->open(key, "recently-detached"_utf16));
    time = UnixDateTime::from_nanoseconds_since_epoch(2'000);
    EXPECT(TRY_OR_FAIL(store->remove(key, "recently-detached"_utf16)));
    auto recent = TRY_OR_FAIL(store->open(key, "recent"_utf16));
    auto recent_detached = TRY_OR_FAIL(store->open(key, "recent-detached"_utf16));
    EXPECT(TRY_OR_FAIL(store->remove(key, "recent-detached"_utf16)));

    auto invalidated = TRY_OR_FAIL(store->clear_accessed_since(time));
    EXPECT_EQ(invalidated.size(), 3u);
    EXPECT_EQ(invalidated[0], recently_detached);
    EXPECT_EQ(invalidated[1], recent);
    EXPECT_EQ(invalidated[2], recent_detached);
    EXPECT(store->read(key, recent).is_error());
    EXPECT(store->commit(key, recent_detached, 0, "data"sv.bytes()).is_error());
    EXPECT(store->release(key, recent_detached).is_error());
    TRY_OR_FAIL(store->read(key, older));
    TRY_OR_FAIL(store->read(key, older_detached));
    auto reopened = TRY_OR_FAIL(store->open(key, "recent"_utf16));
    EXPECT_NE(reopened, recent);
}

TEST_CASE(clear_since_tracks_only_successful_cache_uses)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto time = UnixDateTime::from_nanoseconds_since_epoch(1'000);
    store->set_clock_for_testing([&] { return time; });
    auto key = "partition"_string;
    auto opened = TRY_OR_FAIL(store->open(key, "opened"_utf16));
    auto found = TRY_OR_FAIL(store->open(key, "found"_utf16));
    auto read = TRY_OR_FAIL(store->open(key, "read"_utf16));
    auto committed = TRY_OR_FAIL(store->open(key, "committed"_utf16));
    auto untouched = TRY_OR_FAIL(store->open(key, "untouched"_utf16));
    auto failed_commit = TRY_OR_FAIL(store->open(key, "failed"_utf16));

    // These times differ by one nanosecond, so timestamp truncation cannot silently clear older entries.
    time = UnixDateTime::from_nanoseconds_since_epoch(1'001);
    EXPECT_EQ(TRY_OR_FAIL(store->open(key, "opened"_utf16)), opened);
    EXPECT_EQ(*TRY_OR_FAIL(store->find(key, "found"_utf16)), found);
    TRY_OR_FAIL(store->read(key, read));
    EXPECT_EQ(TRY_OR_FAIL(store->commit(key, committed, 0, "value"sv.bytes())), 1u);
    EXPECT(store->commit(key, failed_commit, 1, "value"sv.bytes()).is_error());
    EXPECT(TRY_OR_FAIL(store->has(key, "untouched"_utf16)));
    EXPECT_EQ(TRY_OR_FAIL(store->list(key)).size(), 6u);
    auto enumerated = TRY_OR_FAIL(store->enumerate(key));
    for (auto id : enumerated)
        TRY_OR_FAIL(store->release(key, id));

    auto invalidated = TRY_OR_FAIL(store->clear_accessed_since(time));
    EXPECT_EQ(invalidated.size(), 4u);
    EXPECT_EQ(invalidated[0], opened);
    EXPECT_EQ(invalidated[1], found);
    EXPECT_EQ(invalidated[2], read);
    EXPECT_EQ(invalidated[3], committed);
    TRY_OR_FAIL(store->read(key, untouched));
    TRY_OR_FAIL(store->read(key, failed_commit));
    EXPECT_EQ(TRY_OR_FAIL(store->clear_accessed_since(UnixDateTime::earliest())).size(), 2u);
    EXPECT(TRY_OR_FAIL(store->clear_accessed_since(UnixDateTime::earliest())).is_empty());
}

TEST_CASE(estimate_since_counts_logical_charge_without_touching_access_times)
{
    auto store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    auto time = UnixDateTime::from_nanoseconds_since_epoch(1'000);
    store->set_clock_for_testing([&] { return time; });
    auto key = "a"_string;
    auto empty = TRY_OR_FAIL(store->estimate_accessed_since(time));
    EXPECT_EQ(empty.total, 0u);
    EXPECT_EQ(empty.since_requested_time, 0u);

    auto old = TRY_OR_FAIL(store->open(key, "o"_utf16));
    TRY_OR_FAIL(store->commit(key, old, 0, "old"sv.bytes()));
    time = UnixDateTime::from_nanoseconds_since_epoch(1'001);
    auto recent = TRY_OR_FAIL(store->open(key, "n"_utf16));
    TRY_OR_FAIL(store->commit(key, recent, 0, "new!"sv.bytes()));
    auto detached = TRY_OR_FAIL(store->open(key, "d"_utf16));
    TRY_OR_FAIL(store->commit(key, detached, 0, "xx"sv.bytes()));
    EXPECT(TRY_OR_FAIL(store->remove(key, "d"_utf16)));

    auto cutoff = time;
    time = UnixDateTime::from_nanoseconds_since_epoch(1'002);
    // Each entry has 256 bytes of fixed charge, one key byte, and one name byte.
    auto sizes = TRY_OR_FAIL(store->estimate_accessed_since(cutoff));
    EXPECT_EQ(sizes.total, 261u + 262u + 260u);
    EXPECT_EQ(sizes.since_requested_time, 262u + 260u);
    EXPECT_EQ(TRY_OR_FAIL(store->estimate_accessed_since(time)).since_requested_time, 0u);
    auto cleared = TRY_OR_FAIL(store->clear_accessed_since(cutoff));
    EXPECT_EQ(cleared.size(), 2u);
    EXPECT_EQ(cleared[0], recent);
    EXPECT_EQ(cleared[1], detached);
    sizes = TRY_OR_FAIL(store->estimate_accessed_since(cutoff));
    EXPECT_EQ(sizes.total, 261u);
    EXPECT_EQ(sizes.since_requested_time, 0u);
    EXPECT(TRY_OR_FAIL(store->has(key, "o"_utf16)));
}

#if defined(AK_OS_LINUX)
TEST_CASE(file_backed_store_recovers_visible_caches_and_discards_old_leases)
{
    Test::TemporaryDirectory directory { "TestCacheStorageStore"sv };
    auto path = directory.path().string();
    auto key = "partition"_string;
    u64 visible_id = 0;
    u64 detached_id = 0;
    {
        auto store = TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
        store->set_clock_for_testing([] { return UnixDateTime::from_nanoseconds_since_epoch(42); });
        visible_id = TRY_OR_FAIL(store->open(key, "visible"_utf16));
        EXPECT_EQ(TRY_OR_FAIL(store->commit(key, visible_id, 0, "snapshot"sv.bytes())), 1u);
        detached_id = TRY_OR_FAIL(store->open(key, "detached"_utf16));
        EXPECT(TRY_OR_FAIL(store->remove(key, "detached"_utf16)));
    }
    {
        auto store = TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
        EXPECT(store->read(key, visible_id).is_error());
        EXPECT(store->read(key, detached_id).is_error());
        EXPECT(TRY_OR_FAIL(store->clear_accessed_since(UnixDateTime::from_nanoseconds_since_epoch(43))).is_empty());
        auto names = TRY_OR_FAIL(store->list(key));
        EXPECT_EQ(names.size(), 1u);
        EXPECT_EQ(names[0], "visible"_utf16);
        auto found = TRY_OR_FAIL(store->find(key, "visible"_utf16));
        EXPECT(found.has_value());
        EXPECT_EQ(*found, visible_id);
        auto snapshot = TRY_OR_FAIL(store->read(key, visible_id));
        EXPECT_EQ(snapshot.revision, 1u);
        EXPECT_EQ(StringView { snapshot.bytes }, "snapshot"sv);
        auto next = TRY_OR_FAIL(store->open(key, "new"_utf16));
        EXPECT(next > detached_id);
        EXPECT(store->commit(key, detached_id, 0, "stale"sv.bytes()).is_error());
    }
}

TEST_CASE(file_backed_store_locks_its_profile_for_its_lifetime)
{
    Test::TemporaryDirectory first_directory { "TestCacheStorageStore"sv };
    Test::TemporaryDirectory other_directory { "TestCacheStorageStore"sv };
    auto path = first_directory.path().string();
    {
        auto first = TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
        auto id = TRY_OR_FAIL(first->open("partition"_string, "name"_utf16));
        auto refused = CacheStorageStore::create_file_backed(path);
        EXPECT(refused.is_error());
        EXPECT(refused.error().is_errno());
        EXPECT_EQ(refused.error().code(), EBUSY);
        TRY_OR_FAIL(first->read("partition"_string, id));
        TRY_OR_FAIL(CacheStorageStore::create_file_backed(other_directory.path().string()));
    }
    TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
}

TEST_CASE(file_backed_store_rejects_untrusted_directory_and_symlink)
{
    Test::TemporaryDirectory directory { "TestCacheStorageStore"sv };
    auto path = directory.path().string();
    VERIFY(chmod(path.characters(), 0755) == 0);
    auto refused = CacheStorageStore::create_file_backed(path);
    EXPECT(refused.is_error());
    EXPECT_EQ(refused.error().code(), EACCES);
    VERIFY(chmod(path.characters(), 0700) == 0);

    auto target = LexicalPath::join(path, "unrelated"sv).string();
    auto file = TRY_OR_FAIL(Core::File::open(target, Core::File::OpenMode::Write));
    TRY_OR_FAIL(file->write_until_depleted("keep"sv.bytes()));
    file->close();
    auto database_path = LexicalPath::join(path, "CacheStorage.db"sv).string();
    VERIFY(symlink(target.characters(), database_path.characters()) == 0);
    EXPECT(CacheStorageStore::create_file_backed(path).is_error());
    auto unchanged = TRY_OR_FAIL(Core::File::open(target, Core::File::OpenMode::Read));
    auto contents = TRY_OR_FAIL(unchanged->read_until_eof());
    EXPECT_EQ(StringView { contents }, "keep"sv);
}

TEST_CASE(file_backed_store_refuses_a_newer_schema_without_changing_its_rows)
{
    Test::TemporaryDirectory directory { "TestCacheStorageStore"sv };
    auto path = directory.path().string();
    {
        auto store = TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
        auto id = TRY_OR_FAIL(store->open("partition"_string, "name"_utf16));
        TRY_OR_FAIL(store->commit("partition"_string, id, 0, "snapshot"sv.bytes()));
    }
    {
        auto database = TRY_OR_FAIL(Database::Database::create(path, "CacheStorage"sv));
        TRY_OR_FAIL(database->execute_raw("UPDATE SchemaVersions SET version = 2 WHERE store = 'CacheStorage';"));
    }
    EXPECT(CacheStorageStore::create_file_backed(path).is_error());
    {
        auto database = TRY_OR_FAIL(Database::Database::create(path, "CacheStorage"sv));
        auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT revision, snapshot FROM CacheStorage;"sv));
        size_t rows = 0;
        TRY_OR_FAIL(database->try_execute_bound_statement(statement, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            ++rows;
            EXPECT_EQ(TRY(row.read_integer<i64>("revision"sv)), 1);
            auto snapshot = TRY(row.read_blob("snapshot"sv, 32));
            EXPECT_EQ(StringView { snapshot }, "snapshot"sv);
            return {}; }));
        EXPECT_EQ(rows, 1u);
    }
}

TEST_CASE(file_backed_store_does_not_recover_a_schema_it_cannot_prepare)
{
    Test::TemporaryDirectory directory { "TestCacheStorageStore"sv };
    auto path = directory.path().string();
    {
        auto store = TRY_OR_FAIL(CacheStorageStore::create_file_backed(path));
        TRY_OR_FAIL(store->open("partition"_string, "name"_utf16));
        EXPECT(TRY_OR_FAIL(store->remove("partition"_string, "name"_utf16)));
    }
    {
        auto database = TRY_OR_FAIL(Database::Database::create(path, "CacheStorage"sv));
        TRY_OR_FAIL(database->execute_raw("ALTER TABLE CacheStorage RENAME COLUMN snapshot TO unexpected_snapshot;"));
    }
    EXPECT(CacheStorageStore::create_file_backed(path).is_error());
    {
        auto database = TRY_OR_FAIL(Database::Database::create(path, "CacheStorage"sv));
        auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT visible, leases FROM CacheStorage;"sv));
        size_t rows = 0;
        TRY_OR_FAIL(database->try_execute_bound_statement(statement, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            ++rows;
            EXPECT_EQ(TRY(row.read_integer<i64>("visible"sv)), 0);
            EXPECT_EQ(TRY(row.read_integer<i64>("leases"sv)), 1);
            return {}; }));
        EXPECT_EQ(rows, 1u);
    }
}

TEST_CASE(private_session_refuses_a_file_backed_store)
{
    Test::TemporaryDirectory private_directory { "TestCacheStorageStore"sv };
    Test::TemporaryDirectory ordinary_directory { "TestCacheStorageStore"sv };

    auto private_session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    auto file_backed_store = TRY_OR_FAIL(CacheStorageStore::create_file_backed(private_directory.path().string()));
    EXPECT(file_backed_store->is_file_backed());
    EXPECT(!private_session->install_cache_storage_store(move(file_backed_store)));
    EXPECT(!private_session->cache_storage_store());

    auto memory_backed_store = TRY_OR_FAIL(CacheStorageStore::create_memory_backed());
    EXPECT(!memory_backed_store->is_file_backed());
    EXPECT(private_session->install_cache_storage_store(move(memory_backed_store)));

    auto ordinary_session = WebView::BrowsingSession::create(WebView::IsPrivate::No);
    EXPECT(ordinary_session->install_cache_storage_store(TRY_OR_FAIL(CacheStorageStore::create_file_backed(ordinary_directory.path().string()))));
    EXPECT(ordinary_session->cache_storage_store()->is_file_backed());
}
#endif
