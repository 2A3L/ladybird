/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibDatabase/Forward.h>
#include <LibRequests/CacheSizes.h>
#include <LibWebView/Export.h>

namespace WebView {

// A session-local store. Callers must derive the partition key and authorize every lease in the Hub.
// All methods run on the Hub thread. IDs identify caches, not callers or permissions.
class WEBVIEW_API CacheStorageStore {
    AK_MAKE_NONCOPYABLE(CacheStorageStore);
    AK_MAKE_NONMOVABLE(CacheStorageStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Limits {
        size_t snapshot_bytes { 8 * MiB };
        size_t partition_bytes { 32 * MiB };
        size_t session_bytes { 128 * MiB };
        size_t names_per_partition { 256 };
        size_t name_code_units { 4096 };
        size_t partition_key_bytes { 4096 };
        size_t identities { 2048 };
        size_t leases { 4096 };
        size_t leases_per_partition { 1024 };
    };

    struct Snapshot {
        u64 revision { 0 };
        ByteBuffer bytes;
    };

    static ErrorOr<NonnullOwnPtr<CacheStorageStore>> create_memory_backed(Limits);
    static ErrorOr<NonnullOwnPtr<CacheStorageStore>> create_memory_backed() { return create_memory_backed(Limits {}); }
#if defined(AK_OS_LINUX)
    // The directory must be a private, trusted profile directory. The caller must not use this for a private session.
    static ErrorOr<NonnullOwnPtr<CacheStorageStore>> create_file_backed(ByteString const& directory, Limits);
    static ErrorOr<NonnullOwnPtr<CacheStorageStore>> create_file_backed(ByteString const& directory) { return create_file_backed(directory, Limits {}); }
#endif
    ~CacheStorageStore();

    bool is_file_backed() const { return m_profile_lock.ptr() != nullptr; }

    // Open and find acquire a lease. Deleting a name detaches it; existing leases retain that cache's identity.
    ErrorOr<u64> open(String const& partition_key, Utf16String const& name);
    ErrorOr<Optional<u64>> find(String const& partition_key, Utf16String const& name);
    ErrorOr<bool> has(String const& partition_key, Utf16String const& name);
    ErrorOr<Vector<Utf16String>> list(String const& partition_key);
    ErrorOr<Vector<u64>> enumerate(String const& partition_key);
    ErrorOr<bool> remove(String const& partition_key, Utf16String const& name);
    ErrorOr<Snapshot> read(String const& partition_key, u64 id);
    // An empty snapshot marks a newly created cache; a committed snapshot includes its format header.
    ErrorOr<u64> commit(String const& partition_key, u64 id, u64 expected_revision, ReadonlyBytes snapshot);
    ErrorOr<void> release(String const& partition_key, u64 id);

    // Opening/finding a named cache, reads, commits, and successful removal mark it accessed. Listing, existence
    // checks, enumeration, and releasing do not. Clearing also invalidates leases to detached caches.
    ErrorOr<Vector<u64>> clear_accessed_since(UnixDateTime);
    // Logical quota charge, not the physical database size or process memory usage.
    ErrorOr<Requests::CacheSizes> estimate_accessed_since(UnixDateTime) const;
    void set_clock_for_testing(Function<UnixDateTime()> clock) { m_clock = move(clock); }

private:
    struct Statements;
    struct Metadata;
    static ErrorOr<void> validate_limits(Limits);
    static ErrorOr<NonnullOwnPtr<Statements>> prepare(Database::Database&, bool recover);
    CacheStorageStore(NonnullRefPtr<Database::Database>, NonnullOwnPtr<Statements>, OwnPtr<Core::File>, Limits);

    ErrorOr<void> validate_key(String const&) const;
    ErrorOr<String> encode_name(Utf16String const&) const;
    ErrorOr<Optional<u64>> find_id(String const&, String const&);
    ErrorOr<void> acquire(u64);
    ErrorOr<void> mark_accessed(u64);
    ErrorOr<Metadata> metadata(String const&, u64);
    UnixDateTime now() const { return m_clock ? m_clock() : UnixDateTime::now(); }

    // Declared first so the database and its prepared statements are destroyed before the lock is released.
    OwnPtr<Core::File> m_profile_lock;
    NonnullRefPtr<Database::Database> m_database;
    NonnullOwnPtr<Statements> m_statements;
    Limits m_limits;
    Function<UnixDateTime()> m_clock;
};

}
