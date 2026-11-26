/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWebView/CacheStorageLeaseRegistry.h>
#include <LibWebView/CacheStorageStore.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <errno.h>

namespace WebView {

using namespace Web::ServiceWorker::CacheStorageIPC;

CacheStorageLeaseRegistry& CacheStorageLeaseRegistry::the()
{
    static auto* registry = new CacheStorageLeaseRegistry;
    return *registry;
}

static StorageError storage_error(Error const& error)
{
    if (error.is_errno()) {
        if (error.code() == ENOSPC)
            return StorageError::QuotaExceeded;
        if (error.code() == EAGAIN)
            return StorageError::StaleRevision;
        if (error.code() == ENOENT)
            return StorageError::Missing;
        if (error.code() == EINVAL)
            return StorageError::Denied;
    }
    return StorageError::Unavailable;
}

Optional<size_t> CacheStorageLeaseRegistry::find_lease(CacheStorageOwner owner, BrowsingSession const& session, CanonicalEnvironmentSettingsObject const& environment, Optional<String> const& partition_key, u64 id) const
{
    for (size_t i = 0; i < m_leases.size(); ++i) {
        auto const& lease = m_leases[i];
        if (!lease.revoked && lease.owner == owner && lease.session.ptr() == &session && lease.environment == &environment && (!partition_key.has_value() || lease.partition_key == *partition_key) && lease.id == id)
            return i;
    }
    return {};
}

void CacheStorageLeaseRegistry::retry_revoked()
{
    for (size_t i = 0; i < m_leases.size();) {
        auto& lease = m_leases[i];
        if (!lease.revoked) {
            ++i;
            continue;
        }
        auto session = lease.session.strong_ref();
        if (session && session->cache_storage_store()) {
            auto result = session->cache_storage_store()->release(lease.partition_key, lease.id);
            if (result.is_error()) {
                ++i;
                continue;
            }
        }
        m_leases.remove(i);
    }
}

void CacheStorageLeaseRegistry::remove_owner(CacheStorageOwner owner)
{
    for (auto& lease : m_leases) {
        if (lease.owner == owner)
            lease.revoked = true;
    }
    retry_revoked();
}

void CacheStorageLeaseRegistry::remove_environment(CanonicalEnvironmentSettingsObject const& environment)
{
    for (auto& lease : m_leases) {
        if (lease.environment == &environment)
            lease.revoked = true;
    }
    retry_revoked();
}

ErrorOr<void> CacheStorageLeaseRegistry::clear_accessed_since(BrowsingSession& session, UnixDateTime since)
{
    if (!session.cache_storage_store())
        return {};
    auto invalidated = TRY(session.cache_storage_store()->clear_accessed_since(since));
    remove_invalidated(session, invalidated.span());
    return {};
}

void CacheStorageLeaseRegistry::remove_invalidated(BrowsingSession& session, ReadonlySpan<u64> ids)
{
    // The store already removed these IDs transactionally; no release is owed for them.
    m_leases.remove_all_matching([&](auto const& lease) {
        return lease.session.ptr() == &session && ids.contains_slow(lease.id);
    });
}

Reply CacheStorageLeaseRegistry::dispatch(CacheStorageOwner owner, BrowsingSession& session, CanonicalEnvironmentSettingsObject const& environment, Optional<String> const& optional_partition_key, Request const& request)
{
    retry_revoked();
    if (!session.cache_storage_store())
        return StorageError::Unavailable;

    // Release is authorized by ownership (endpoint, environment, session and lease ID) alone. The partition key is
    // neither required nor compared, and the store is addressed with the key the lease was issued under.
    if (auto const* release = request.get_pointer<Release>()) {
        auto index = find_lease(owner, session, environment, {}, release->id.value());
        if (!index.has_value())
            return StorageError::Denied;
        auto result = session.cache_storage_store()->release(m_leases[*index].partition_key, release->id.value());
        if (result.is_error())
            return storage_error(result.error());
        m_leases.remove(*index);
        return Released {};
    }

    if (!optional_partition_key.has_value())
        return StorageError::Denied;
    auto const& partition_key = *optional_partition_key;

    // A reused environment must not retain a lease for its previous session or partition.
    for (auto& lease : m_leases) {
        if (lease.owner == owner && lease.environment == &environment && (lease.session.ptr() != &session || lease.partition_key != partition_key))
            lease.revoked = true;
    }
    retry_revoked();

    auto& store = *session.cache_storage_store();
    auto issue = [&](u64 id) {
        m_leases.unchecked_append(Lease { owner, session.make_weak_ptr(), &environment, partition_key, id });
    };
    auto reserve_leases = [&](size_t count) -> bool {
        if (m_leases.size() > NumericLimits<size_t>::max() - count)
            return false;
        return !m_leases.try_ensure_capacity(m_leases.size() + count).is_error();
    };

    return request.visit(
        [&](Open const& open) -> Reply {
            if (!reserve_leases(1))
                return StorageError::Unavailable;
            auto result = store.open(partition_key, open.name.value);
            if (result.is_error())
                return storage_error(result.error());
            issue(result.value());
            return Opened { CacheId { result.value() } };
        },
        [&](Lookup const& lookup) -> Reply {
            if (!reserve_leases(1))
                return StorageError::Unavailable;
            auto result = store.find(partition_key, lookup.name.value);
            if (result.is_error())
                return storage_error(result.error());
            Optional<CacheId> id;
            if (result.value().has_value()) {
                issue(*result.value());
                id = CacheId { *result.value() };
            }
            return Found { move(id) };
        },
        [&](List const&) -> Reply {
            auto result = store.list(partition_key);
            if (result.is_error())
                return storage_error(result.error());
            if (result.value().size() > maximum_name_count)
                return StorageError::Unavailable;
            Names names;
            if (names.names.try_ensure_capacity(result.value().size()).is_error())
                return StorageError::Unavailable;
            for (auto& name : result.value()) {
                if (name.length_in_code_units() > maximum_name_length)
                    return StorageError::Unavailable;
                names.names.unchecked_append(Name { move(name) });
            }
            return names;
        },
        [&](Delete const& request_delete) -> Reply {
            auto result = store.remove(partition_key, request_delete.name.value);
            if (result.is_error())
                return storage_error(result.error());
            return Deleted { result.value() };
        },
        [&](Read const& read) -> Reply {
            if (!find_lease(owner, session, environment, partition_key, read.id.value()).has_value())
                return StorageError::Denied;
            auto result = store.read(partition_key, read.id.value());
            if (result.is_error())
                return storage_error(result.error());
            auto snapshot = result.release_value();
            if (snapshot.bytes.size() > maximum_snapshot_size)
                return StorageError::Unavailable;
            return Loaded { Revision { snapshot.revision }, Snapshot { move(snapshot.bytes) } };
        },
        [&](Commit const& commit) -> Reply {
            if (!find_lease(owner, session, environment, partition_key, commit.id.value()).has_value())
                return StorageError::Denied;
            if (commit.snapshot.bytes.size() > maximum_snapshot_size)
                return StorageError::QuotaExceeded;
            auto result = store.commit(partition_key, commit.id.value(), commit.expected_revision.value(), commit.snapshot.bytes.bytes());
            if (result.is_error())
                return storage_error(result.error());
            return Committed { Revision { result.value() } };
        },
        [&](Release const&) -> Reply {
            VERIFY_NOT_REACHED();
        },
        [&](Has const& has) -> Reply {
            auto result = store.has(partition_key, has.name.value);
            if (result.is_error())
                return storage_error(result.error());
            return Exists { result.value() };
        },
        [&](Enumerate const&) -> Reply {
            CacheIds ids;
            if (!reserve_leases(maximum_name_count) || ids.ids.try_ensure_capacity(maximum_name_count).is_error())
                return StorageError::Unavailable;
            auto result = store.enumerate(partition_key);
            if (result.is_error())
                return storage_error(result.error());
            if (result.value().size() > maximum_name_count) {
                for (auto id : result.value())
                    (void)store.release(partition_key, id);
                return StorageError::Unavailable;
            }
            for (auto id : result.value()) {
                issue(id);
                ids.ids.unchecked_append(CacheId { id });
            }
            return ids;
        });
}

}
