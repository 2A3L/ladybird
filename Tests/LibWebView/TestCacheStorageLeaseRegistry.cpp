/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CacheStorageLeaseRegistry.h>
#include <LibWebView/CacheStorageStore.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>

using namespace Web::ServiceWorker::CacheStorageIPC;
using WebView::CacheStorageOwner;

class TestCacheEnvironment final : public WebView::CanonicalEnvironmentSettingsObject {
public:
    explicit TestCacheEnvironment(Web::HTML::EnvironmentId id)
        : CanonicalEnvironmentSettingsObject(move(id))
        , m_origin(URL::Origin::create_opaque())
    {
    }
    virtual URL::Origin const& origin() const override { return m_origin; }
    virtual Optional<URL::URL> top_level_creation_url() const override { return {}; }
    virtual Optional<URL::Origin> top_level_origin() const override { return {}; }

private:
    URL::Origin m_origin;
};

static Snapshot snapshot_of(StringView contents)
{
    return Snapshot { MUST(ByteBuffer::copy(contents.bytes())) };
}

TEST_CASE(cache_leases_are_bound_to_endpoint_environment_session_and_partition)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto another_session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(another_session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int owner_identity = 0;
    int other_identity = 0;
    CacheStorageOwner owner { CacheStorageOwner::Kind::Page, &owner_identity };
    CacheStorageOwner other { CacheStorageOwner::Kind::Page, &other_identity };
    auto shared_id = Web::HTML::EnvironmentId::generate();
    TestCacheEnvironment environment { shared_id };
    TestCacheEnvironment other_environment { shared_id };
    auto key = "partition-a"_string;
    auto other_key = "partition-b"_string;

    auto opened = registry.dispatch(owner, *session, environment, key, Open { { "cache"_utf16 } });
    VERIFY(opened.has<Opened>());
    auto id = opened.get<Opened>().id;
    auto read = Request { Read { id } };
    EXPECT(registry.dispatch(other, *session, environment, key, read).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, other_environment, key, read).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, environment, key, read).has<Loaded>());
    EXPECT(registry.dispatch(owner, *another_session, environment, key, read).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, environment, other_key, read).has<StorageError>());
    // Changing this endpoint's partition revokes the old lease, even if it later changes back.
    EXPECT(registry.dispatch(owner, *session, environment, key, read).has<StorageError>());
    registry.remove_owner(owner);
    registry.remove_owner(other);
}

TEST_CASE(cache_leases_count_repeated_acquisitions_and_are_revoked_on_teardown)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int owner_identity = 0;
    CacheStorageOwner owner { CacheStorageOwner::Kind::Worker, &owner_identity };
    TestCacheEnvironment environment { Web::HTML::EnvironmentId::generate() };
    auto key = "partition"_string;
    auto first = registry.dispatch(owner, *session, environment, key, Open { { "cache"_utf16 } });
    auto second = registry.dispatch(owner, *session, environment, key, Lookup { { "cache"_utf16 } });
    VERIFY(first.has<Opened>());
    VERIFY(second.has<Found>() && second.get<Found>().id.has_value());
    auto id = first.get<Opened>().id;
    EXPECT_EQ(*second.get<Found>().id, id);
    EXPECT(registry.dispatch(owner, *session, environment, key, Release { id }).has<Released>());
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { id }).has<Loaded>());
    registry.remove_environment(environment);
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { id }).has<StorageError>());
    registry.remove_owner(owner);
}

TEST_CASE(cache_leases_are_released_by_ownership_without_a_partition_key)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int owner_identity = 0;
    int other_identity = 0;
    CacheStorageOwner owner { CacheStorageOwner::Kind::Page, &owner_identity };
    CacheStorageOwner other { CacheStorageOwner::Kind::Page, &other_identity };
    TestCacheEnvironment environment { Web::HTML::EnvironmentId::generate() };
    TestCacheEnvironment other_environment { Web::HTML::EnvironmentId::generate() };
    auto key = "partition-a"_string;
    auto other_key = "partition-b"_string;

    auto open = [&](Utf16String const& name) {
        auto opened = registry.dispatch(owner, *session, environment, key, Open { { name } });
        VERIFY(opened.has<Opened>());
        return opened.get<Opened>().id;
    };
    auto first_id = open("first"_utf16);

    // Another endpoint or environment cannot release the lease, with or without a key.
    EXPECT(registry.dispatch(other, *session, environment, {}, Release { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(other, *session, environment, key, Release { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, other_environment, {}, Release { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, other_environment, key, Release { first_id }).has<StorageError>());

    // Read and Commit still require the partition key the lease was issued under.
    EXPECT(registry.dispatch(owner, *session, environment, {}, Read { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, environment, {}, Commit { first_id, Revision { 0 }, snapshot_of("data"sv) }).has<StorageError>());

    // The owner can release without a key, even though the document is no longer active.
    EXPECT(registry.dispatch(owner, *session, environment, {}, Release { first_id }).has<Released>());
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(owner, *session, environment, {}, Release { first_id }).has<StorageError>());

    // A different key does not prevent the release either.
    auto second_id = open("second"_utf16);
    EXPECT(registry.dispatch(owner, *session, environment, other_key, Release { second_id }).has<Released>());
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { second_id }).has<StorageError>());
    registry.remove_owner(owner);
    registry.remove_owner(other);
}

TEST_CASE(cache_leases_are_revoked_when_the_same_endpoint_changes_partition)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int owner_identity = 0;
    CacheStorageOwner owner { CacheStorageOwner::Kind::Page, &owner_identity };
    TestCacheEnvironment environment { Web::HTML::EnvironmentId::generate() };
    auto key = "partition-a"_string;
    auto other_key = "partition-b"_string;

    auto opened = registry.dispatch(owner, *session, environment, key, Open { { "cache"_utf16 } });
    VERIFY(opened.has<Opened>());
    auto id = opened.get<Opened>().id;
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { id }).has<Loaded>());

    // Only the partition changes: the session, owner and environment stay the same.
    EXPECT(registry.dispatch(owner, *session, environment, other_key, List {}).has<Names>());
    EXPECT(registry.dispatch(owner, *session, environment, key, Read { id }).has<StorageError>());
    registry.remove_owner(owner);
}

TEST_CASE(cache_leases_commit_snapshots_and_reject_stale_revisions)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int owner_identity = 0;
    CacheStorageOwner owner { CacheStorageOwner::Kind::Page, &owner_identity };
    TestCacheEnvironment environment { Web::HTML::EnvironmentId::generate() };
    auto key = "partition"_string;

    auto opened = registry.dispatch(owner, *session, environment, key, Open { { "cache"_utf16 } });
    VERIFY(opened.has<Opened>());
    auto id = opened.get<Opened>().id;

    auto committed = registry.dispatch(owner, *session, environment, key, Commit { id, Revision { 0 }, snapshot_of("first"sv) });
    VERIFY(committed.has<Committed>());
    EXPECT_EQ(committed.get<Committed>().revision.value(), 1u);

    auto loaded = registry.dispatch(owner, *session, environment, key, Read { id });
    VERIFY(loaded.has<Loaded>());
    EXPECT_EQ(loaded.get<Loaded>().revision.value(), 1u);
    EXPECT_EQ(StringView { loaded.get<Loaded>().snapshot.bytes.bytes() }, "first"sv);

    auto stale = registry.dispatch(owner, *session, environment, key, Commit { id, Revision { 0 }, snapshot_of("second"sv) });
    VERIFY(stale.has<StorageError>());
    EXPECT(stale.get<StorageError>() == StorageError::StaleRevision);
    registry.remove_owner(owner);
}

TEST_CASE(shared_cache_store_is_not_activated_by_default)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    int owner_identity = 0;
    TestCacheEnvironment environment { Web::HTML::EnvironmentId::generate() };
    auto result = WebView::CacheStorageLeaseRegistry::the().dispatch({ CacheStorageOwner::Kind::Page, &owner_identity }, *session, environment, "partition"_string, List {});
    VERIFY(result.has<StorageError>());
    EXPECT(result.get<StorageError>() == StorageError::Unavailable);
}

TEST_CASE(clearing_site_data_revokes_only_the_selected_session_and_cutoff)
{
    auto first_session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    auto second_session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(first_session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    VERIFY(second_session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int first_identity = 0;
    int second_identity = 0;
    CacheStorageOwner first { CacheStorageOwner::Kind::Page, &first_identity };
    CacheStorageOwner second { CacheStorageOwner::Kind::Page, &second_identity };
    TestCacheEnvironment first_environment { Web::HTML::EnvironmentId::generate() };
    TestCacheEnvironment second_environment { Web::HTML::EnvironmentId::generate() };
    auto key = "partition"_string;
    auto first_result = registry.dispatch(first, *first_session, first_environment, key, Open { { "cache"_utf16 } });
    auto second_result = registry.dispatch(second, *second_session, second_environment, key, Open { { "cache"_utf16 } });
    VERIFY(first_result.has<Opened>() && second_result.has<Opened>());
    auto first_id = first_result.get<Opened>().id;
    auto second_id = second_result.get<Opened>().id;

    MUST(registry.clear_accessed_since(*first_session, UnixDateTime::latest()));
    EXPECT(registry.dispatch(first, *first_session, first_environment, key, Read { first_id }).has<Loaded>());
    MUST(registry.clear_accessed_since(*first_session, UnixDateTime::earliest()));
    EXPECT(registry.dispatch(first, *first_session, first_environment, key, Read { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(first, *first_session, first_environment, key, Commit { first_id, Revision { 0 }, Snapshot {} }).has<StorageError>());
    EXPECT(registry.dispatch(second, *second_session, second_environment, key, Read { second_id }).has<Loaded>());
    registry.remove_owner(first);
    registry.remove_owner(second);
}

TEST_CASE(environment_teardown_does_not_revoke_another_canonical_environment_with_the_same_wire_id)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto& registry = WebView::CacheStorageLeaseRegistry::the();
    int first_identity = 0;
    int second_identity = 0;
    CacheStorageOwner first { CacheStorageOwner::Kind::Page, &first_identity };
    CacheStorageOwner second { CacheStorageOwner::Kind::Page, &second_identity };
    auto shared_id = Web::HTML::EnvironmentId::generate();
    TestCacheEnvironment first_environment { shared_id };
    TestCacheEnvironment second_environment { shared_id };
    auto key = "partition"_string;
    auto first_result = registry.dispatch(first, *session, first_environment, key, Open { { "first"_utf16 } });
    auto second_result = registry.dispatch(second, *session, second_environment, key, Open { { "second"_utf16 } });
    VERIFY(first_result.has<Opened>() && second_result.has<Opened>());
    auto first_id = first_result.get<Opened>().id;
    auto second_id = second_result.get<Opened>().id;
    registry.remove_environment(first_environment);
    EXPECT(registry.dispatch(first, *session, first_environment, key, Read { first_id }).has<StorageError>());
    EXPECT(registry.dispatch(second, *session, second_environment, key, Read { second_id }).has<Loaded>());
    registry.remove_owner(first);
    registry.remove_owner(second);
}

TEST_CASE(a_session_cannot_replace_its_cache_store)
{
    auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
    VERIFY(session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    auto* original = session->cache_storage_store();
    EXPECT(!session->install_cache_storage_store(MUST(WebView::CacheStorageStore::create_memory_backed())));
    EXPECT(session->cache_storage_store() == original);
}
