/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>
#include <LibWebView/CacheStorageStore.h>

#if defined(AK_OS_LINUX)
#    include <fcntl.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace WebView {

struct CacheStorageStore::Statements {
    AK_ALLOC_WITH_KMALLOC;
    Database::StatementID find { 0 };
    Database::StatementID statistics { 0 };
    Database::StatementID lease_statistics { 0 };
    Database::StatementID insert { 0 };
    Database::StatementID acquire { 0 };
    Database::StatementID acquire_visible { 0 };
    Database::StatementID mark_accessed { 0 };
    Database::StatementID list { 0 };
    Database::StatementID enumerate { 0 };
    Database::StatementID detach { 0 };
    Database::StatementID remove_unleased { 0 };
    Database::StatementID read { 0 };
    Database::StatementID metadata { 0 };
    Database::StatementID update { 0 };
    Database::StatementID release { 0 };
    Database::StatementID find_accessed_since { 0 };
    Database::StatementID clear_accessed_since { 0 };
    Database::StatementID estimate_accessed_since { 0 };
};

struct CacheStorageStore::Metadata {
    u64 revision { 0 };
    size_t size { 0 };
};

struct StoreStatistics {
    size_t identities { 0 };
    size_t leases { 0 };
    size_t session_bytes { 0 };
    size_t names { 0 };
    size_t partition_bytes { 0 };
    size_t partition_leases { 0 };
};

struct LeaseStatistics {
    size_t leases;
    size_t partition_leases;
};

static ErrorOr<void> bind_time(auto& bind, UnixDateTime time)
{
    auto timestamp = time.offset_to_epoch().to_timespec();
    TRY(bind("seconds"sv, static_cast<i64>(timestamp.tv_sec)));
    return bind("nanoseconds"sv, static_cast<i64>(timestamp.tv_nsec));
}

static ErrorOr<StoreStatistics> statistics(Database::Database& database, Database::StatementID statement, String const& key)
{
    return database.try_execute_bound_statement_one<StoreStatistics>(statement, [&](auto& bind) -> ErrorOr<void> { return bind("key"sv, key.bytes()); }, [&](Database::ResultRow& row) -> ErrorOr<StoreStatistics> { return StoreStatistics {
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("identities"sv)),
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("leases"sv)),
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("session_bytes"sv)),
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("names"sv)),
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("partition_bytes"sv)),
                                                                                                                                                                                                                         TRY(row.read_integer<size_t>("partition_leases"sv)),
                                                                                                                                                                                                                     }; });
}

static ErrorOr<LeaseStatistics> lease_statistics(Database::Database& database, Database::StatementID statement, String const& key)
{
    auto read_counts = [](Database::ResultRow& row) -> ErrorOr<LeaseStatistics> {
        return LeaseStatistics { TRY(row.read_integer<size_t>("leases"sv)), TRY(row.read_integer<size_t>("partition_leases"sv)) };
    };
    return database.try_execute_bound_statement_one<LeaseStatistics>(statement, [&](auto& bind) -> ErrorOr<void> { return bind("key"sv, key.bytes()); }, read_counts);
}

ErrorOr<void> CacheStorageStore::validate_limits(Limits limits)
{
    Limits maximum;
    if (limits.snapshot_bytes > maximum.snapshot_bytes || limits.partition_bytes > maximum.partition_bytes
        || limits.session_bytes > maximum.session_bytes || limits.names_per_partition > maximum.names_per_partition
        || limits.name_code_units > maximum.name_code_units || limits.partition_key_bytes > maximum.partition_key_bytes
        || limits.identities > maximum.identities || limits.leases > maximum.leases
        || limits.leases_per_partition > maximum.leases_per_partition)
        return Error::from_errno(EINVAL);
    return {};
}

ErrorOr<NonnullOwnPtr<CacheStorageStore>> CacheStorageStore::create_memory_backed(Limits limits)
{
    TRY(validate_limits(limits));
    auto database = TRY(Database::Database::create_memory_backed());
    auto statements = TRY(prepare(*database, false));
    return adopt_nonnull_own_or_enomem(new (nothrow) CacheStorageStore(move(database), move(statements), nullptr, limits));
}

#if defined(AK_OS_LINUX)
ErrorOr<NonnullOwnPtr<CacheStorageStore>> CacheStorageStore::create_file_backed(ByteString const& directory, Limits limits)
{
    TRY(validate_limits(limits));
    if (directory.is_empty() || directory.contains('\0'))
        return Error::from_errno(EINVAL);
    auto parent = TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes, 0700));
    auto directory_stat = TRY(parent.stat());
    if (directory_stat.st_uid != geteuid() || (directory_stat.st_mode & 0077) != 0)
        return Error::from_errno(EACCES);

    auto lock = TRY(parent.open("CacheStorage.lock"sv, Core::File::OpenMode::ReadWrite | Core::File::OpenMode::NoFollow, 0600));
    auto lock_stat = TRY(lock->stat());
    if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_uid != geteuid() || lock_stat.st_nlink != 1)
        return Error::from_errno(EACCES);
    int result;
    do {
        result = flock(lock->fd(), LOCK_EX | LOCK_NB);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return Error::from_errno(errno == EWOULDBLOCK ? EBUSY : errno);

    // SQLite opens these by path; the private directory protects the checks from other users.
    for (auto name : { "CacheStorage.db"sv, "CacheStorage.db-wal"sv, "CacheStorage.db-shm"sv, "CacheStorage.db-journal"sv }) {
        auto status = Core::System::fstatat(parent.fd(), name, AT_SYMLINK_NOFOLLOW);
        if (status.is_error()) {
            if (status.error().is_errno() && status.error().code() == ENOENT)
                continue;
            return status.release_error();
        }
        if (!S_ISREG(status.value().st_mode) || status.value().st_uid != geteuid() || status.value().st_nlink != 1)
            return Error::from_errno(EACCES);
    }

    auto database = TRY(Database::Database::create(directory, "CacheStorage"sv));
    auto statements = TRY(prepare(*database, true));
    return adopt_nonnull_own_or_enomem(new (nothrow) CacheStorageStore(move(database), move(statements), move(lock), limits));
}
#endif

ErrorOr<NonnullOwnPtr<CacheStorageStore::Statements>> CacheStorageStore::prepare(Database::Database& database, bool recover)
{
    // This schema version is independent of the serialized snapshot format inside each BLOB.
    auto migrations = to_array<Database::Migration>({ {
        .version = 1,
        .sql = R"~~(
        CREATE TABLE CacheStorage (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            partition_key BLOB NOT NULL,
            name BLOB NOT NULL,
            visible INTEGER NOT NULL,
            leases INTEGER NOT NULL,
            revision INTEGER NOT NULL,
            charge INTEGER NOT NULL,
            snapshot BLOB NOT NULL,
            last_access_seconds INTEGER NOT NULL,
            last_access_nanoseconds INTEGER NOT NULL
        );
        CREATE UNIQUE INDEX CacheStorageNames ON CacheStorage(partition_key, name) WHERE visible = 1;
        )~~"sv,
    } });
    if (TRY(database.migrate("CacheStorage"sv, migrations)) == Database::MigrationOutcome::DatabaseTooNew)
        return Error::from_string_literal("Cache storage schema is newer than this build");

    auto statements = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Statements));
    statements->find = TRY(database.prepare_statement("SELECT id FROM CacheStorage WHERE partition_key = :key AND name = :name AND visible = 1;"sv));
    statements->statistics = TRY(database.prepare_statement(R"~~(
        SELECT COUNT(*) AS identities, COALESCE(SUM(leases), 0) AS leases,
            COALESCE(SUM(charge + length(snapshot)), 0) AS session_bytes,
            COUNT(*) FILTER (WHERE partition_key = :key AND visible = 1) AS names,
            COALESCE(SUM(charge + length(snapshot)) FILTER (WHERE partition_key = :key), 0) AS partition_bytes,
            COALESCE(SUM(leases) FILTER (WHERE partition_key = :key), 0) AS partition_leases
        FROM CacheStorage;
    )~~"sv));
    statements->lease_statistics = TRY(database.prepare_statement(R"~~(
        SELECT COALESCE(SUM(leases), 0) AS leases,
            COALESCE(SUM(leases) FILTER (WHERE partition_key = :key), 0) AS partition_leases
        FROM CacheStorage;
    )~~"sv));
    statements->insert = TRY(database.prepare_statement("INSERT INTO CacheStorage (partition_key, name, visible, leases, revision, charge, snapshot, last_access_seconds, last_access_nanoseconds) VALUES (:key, :name, 1, 1, 0, :charge, :snapshot, :seconds, :nanoseconds) RETURNING id;"sv));
    statements->acquire = TRY(database.prepare_statement("UPDATE CacheStorage SET leases = leases + 1 WHERE id = :id;"sv));
    statements->acquire_visible = TRY(database.prepare_statement("UPDATE CacheStorage SET leases = leases + 1 WHERE partition_key = :key AND visible = 1;"sv));
    statements->mark_accessed = TRY(database.prepare_statement("UPDATE CacheStorage SET last_access_seconds = :seconds, last_access_nanoseconds = :nanoseconds WHERE id = :id;"sv));
    statements->list = TRY(database.prepare_statement("SELECT name FROM CacheStorage WHERE partition_key = :key AND visible = 1 ORDER BY id;"sv));
    statements->enumerate = TRY(database.prepare_statement("SELECT id FROM CacheStorage WHERE partition_key = :key AND visible = 1 ORDER BY id;"sv));
    statements->detach = TRY(database.prepare_statement("UPDATE CacheStorage SET visible = 0, last_access_seconds = :seconds, last_access_nanoseconds = :nanoseconds WHERE id = :id;"sv));
    statements->remove_unleased = TRY(database.prepare_statement("DELETE FROM CacheStorage WHERE id = :id AND visible = 0 AND leases = 0;"sv));
    statements->read = TRY(database.prepare_statement("SELECT revision, leases, snapshot FROM CacheStorage WHERE partition_key = :key AND id = :id;"sv));
    statements->metadata = TRY(database.prepare_statement("SELECT revision, leases, length(snapshot) AS size FROM CacheStorage WHERE partition_key = :key AND id = :id;"sv));
    statements->update = TRY(database.prepare_statement("UPDATE CacheStorage SET revision = :revision, snapshot = :snapshot, last_access_seconds = :seconds, last_access_nanoseconds = :nanoseconds WHERE id = :id;"sv));
    statements->release = TRY(database.prepare_statement("UPDATE CacheStorage SET leases = leases - 1 WHERE id = :id;"sv));
    statements->find_accessed_since = TRY(database.prepare_statement("SELECT id FROM CacheStorage WHERE (last_access_seconds, last_access_nanoseconds) >= (:seconds, :nanoseconds) ORDER BY id;"sv));
    statements->clear_accessed_since = TRY(database.prepare_statement("DELETE FROM CacheStorage WHERE (last_access_seconds, last_access_nanoseconds) >= (:seconds, :nanoseconds);"sv));
    statements->estimate_accessed_since = TRY(database.prepare_statement(R"~~(
        SELECT COALESCE(SUM(charge + length(snapshot)), 0) AS total,
            COALESCE(SUM(CASE WHEN (last_access_seconds, last_access_nanoseconds) >= (:seconds, :nanoseconds)
                THEN charge + length(snapshot) ELSE 0 END), 0) AS since_requested_time
        FROM CacheStorage;
    )~~"sv));

    // A reopened store has no clients from its previous process. Only the exclusive lock holder may recover it.
    if (recover) {
        TRY(database.transaction([&]() -> ErrorOr<void> {
            TRY(database.execute_raw("DELETE FROM CacheStorage WHERE visible = 0;"));
            return database.execute_raw("UPDATE CacheStorage SET leases = 0;");
        }));
    }
    return statements;
}

CacheStorageStore::CacheStorageStore(NonnullRefPtr<Database::Database> database, NonnullOwnPtr<Statements> statements, OwnPtr<Core::File> profile_lock, Limits limits)
    : m_profile_lock(move(profile_lock))
    , m_database(move(database))
    , m_statements(move(statements))
    , m_limits(limits)
{
}

CacheStorageStore::~CacheStorageStore() = default;

ErrorOr<void> CacheStorageStore::validate_key(String const& key) const
{
    if (key.is_empty() || key.bytes().size() > m_limits.partition_key_bytes)
        return Error::from_errno(EINVAL);
    return {};
}

ErrorOr<String> CacheStorageStore::encode_name(Utf16String const& name) const
{
    if (name.length_in_code_units() > m_limits.name_code_units)
        return Error::from_errno(ENOSPC);
    StringBuilder builder;
    TRY(builder.try_append(name.utf16_view()));
    return builder.to_string();
}

ErrorOr<Optional<u64>> CacheStorageStore::find_id(String const& key, String const& name)
{
    Optional<u64> id;
    TRY(m_database->try_execute_bound_statement(m_statements->find, 1, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("key"sv, key.bytes()));
            return bind("name"sv, name.bytes()); }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            id = TRY(row.read_integer<u64>("id"sv));
            return {}; }));
    return id;
}

ErrorOr<void> CacheStorageStore::acquire(u64 id)
{
    return m_database->try_execute_bound_statement(m_statements->acquire,
        [&](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(id)); });
}

ErrorOr<void> CacheStorageStore::mark_accessed(u64 id)
{
    return m_database->try_execute_bound_statement(m_statements->mark_accessed, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("id"sv, static_cast<i64>(id)));
        return bind_time(bind, now());
    });
}

ErrorOr<u64> CacheStorageStore::open(String const& key, Utf16String const& name)
{
    TRY(validate_key(key));
    auto encoded_name = TRY(encode_name(name));
    Optional<u64> id;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        auto stats = TRY(statistics(*m_database, m_statements->statistics, key));
        if (stats.leases >= m_limits.leases || stats.partition_leases >= m_limits.leases_per_partition)
            return Error::from_errno(ENOSPC);
        id = TRY(find_id(key, encoded_name));
        if (id.has_value()) {
            TRY(acquire(*id));
            return mark_accessed(*id);
        }
        if (stats.identities >= m_limits.identities || stats.names >= m_limits.names_per_partition)
            return Error::from_errno(ENOSPC);

        Checked<size_t> charge = 256;
        charge += key.bytes().size();
        charge += encoded_name.bytes().size();
        if (charge.has_overflow() || charge.value() > m_limits.partition_bytes || charge.value() > m_limits.session_bytes
            || stats.partition_bytes > m_limits.partition_bytes - charge.value() || stats.session_bytes > m_limits.session_bytes - charge.value())
            return Error::from_errno(ENOSPC);
        auto new_id = TRY(m_database->try_execute_bound_statement_one<u64>(m_statements->insert, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("key"sv, key.bytes()));
            TRY(bind("name"sv, encoded_name.bytes()));
            TRY(bind("charge"sv, static_cast<i64>(charge.value())));
            TRY(bind("snapshot"sv, ReadonlyBytes {}));
            return bind_time(bind, now()); }, [](Database::ResultRow& row) -> ErrorOr<u64> { return row.read_integer<u64>("id"sv); }));
        id = new_id;
        return {};
    }));
    return id.release_value();
}

ErrorOr<Optional<u64>> CacheStorageStore::find(String const& key, Utf16String const& name)
{
    TRY(validate_key(key));
    auto encoded_name = TRY(encode_name(name));
    Optional<u64> id;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        id = TRY(find_id(key, encoded_name));
        if (!id.has_value())
            return {};
        auto stats = TRY(lease_statistics(*m_database, m_statements->lease_statistics, key));
        if (stats.leases >= m_limits.leases || stats.partition_leases >= m_limits.leases_per_partition)
            return Error::from_errno(ENOSPC);
        TRY(acquire(*id));
        return mark_accessed(*id);
    }));
    return id;
}

ErrorOr<bool> CacheStorageStore::has(String const& key, Utf16String const& name)
{
    TRY(validate_key(key));
    return (TRY(find_id(key, TRY(encode_name(name))))).has_value();
}

ErrorOr<Vector<Utf16String>> CacheStorageStore::list(String const& key)
{
    TRY(validate_key(key));
    return m_database->try_collect_bound_statement<Utf16String>(m_statements->list, m_limits.names_per_partition, [&](auto& bind) -> ErrorOr<void> { return bind("key"sv, key.bytes()); }, [&](Database::ResultRow& row) -> ErrorOr<Utf16String> { auto name = TRY(row.read_blob("name"sv, m_limits.name_code_units * 4)); return Utf16String::try_from_utf8(StringView { name }); });
}

ErrorOr<Vector<u64>> CacheStorageStore::enumerate(String const& key)
{
    TRY(validate_key(key));
    Vector<u64> ids;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        ids = TRY(m_database->try_collect_bound_statement<u64>(m_statements->enumerate, m_limits.names_per_partition, [&](auto& bind) -> ErrorOr<void> { return bind("key"sv, key.bytes()); }, [&](Database::ResultRow& row) -> ErrorOr<u64> { return row.read_integer<u64>("id"sv); }));
        auto stats = TRY(lease_statistics(*m_database, m_statements->lease_statistics, key));
        if (ids.size() > m_limits.leases || stats.leases > m_limits.leases - ids.size()
            || ids.size() > m_limits.leases_per_partition || stats.partition_leases > m_limits.leases_per_partition - ids.size())
            return Error::from_errno(ENOSPC);
        if (ids.is_empty())
            return {};
        return m_database->try_execute_bound_statement(m_statements->acquire_visible,
            [&](auto& bind) -> ErrorOr<void> { return bind("key"sv, key.bytes()); });
    }));
    return ids;
}

ErrorOr<bool> CacheStorageStore::remove(String const& key, Utf16String const& name)
{
    TRY(validate_key(key));
    auto encoded_name = TRY(encode_name(name));
    bool removed = false;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        auto id = TRY(find_id(key, encoded_name));
        if (!id.has_value())
            return {};
        auto bind_id = [&](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(*id)); };
        TRY(m_database->try_execute_bound_statement(m_statements->detach, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind_id(bind));
            return bind_time(bind, now());
        }));
        TRY(m_database->try_execute_bound_statement(m_statements->remove_unleased, bind_id));
        removed = true;
        return {};
    }));
    return removed;
}

ErrorOr<CacheStorageStore::Snapshot> CacheStorageStore::read(String const& key, u64 id)
{
    TRY(validate_key(key));
    if (id == 0 || id > static_cast<u64>(NumericLimits<i64>::max()))
        return Error::from_errno(EINVAL);
    Optional<Snapshot> snapshot;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        TRY(m_database->try_execute_bound_statement(m_statements->read, 1, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("key"sv, key.bytes()));
            return bind("id"sv, static_cast<i64>(id)); }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            if (TRY(row.read_integer<u64>("leases"sv)) == 0)
                return Error::from_errno(ENOENT);
            snapshot = Snapshot { TRY(row.read_integer<u64>("revision"sv)), TRY(row.read_blob("snapshot"sv, m_limits.snapshot_bytes)) };
            return {}; }));
        if (!snapshot.has_value())
            return Error::from_errno(ENOENT);
        return mark_accessed(id);
    }));
    return snapshot.release_value();
}

ErrorOr<CacheStorageStore::Metadata> CacheStorageStore::metadata(String const& key, u64 id)
{
    TRY(validate_key(key));
    if (id == 0 || id > static_cast<u64>(NumericLimits<i64>::max()))
        return Error::from_errno(EINVAL);
    Optional<Metadata> result;
    TRY(m_database->try_execute_bound_statement(m_statements->metadata, 1, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("key"sv, key.bytes()));
            return bind("id"sv, static_cast<i64>(id)); }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            if (TRY(row.read_integer<u64>("leases"sv)) == 0)
                return Error::from_errno(ENOENT);
            result = Metadata { TRY(row.read_integer<u64>("revision"sv)), TRY(row.read_integer<size_t>("size"sv)) };
            return {}; }));
    if (!result.has_value())
        return Error::from_errno(ENOENT);
    return result.release_value();
}

ErrorOr<u64> CacheStorageStore::commit(String const& key, u64 id, u64 expected_revision, ReadonlyBytes bytes)
{
    if (bytes.is_empty())
        return Error::from_errno(EINVAL);
    if (bytes.size() > m_limits.snapshot_bytes)
        return Error::from_errno(ENOSPC);
    u64 revision = 0;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        auto previous = TRY(metadata(key, id));
        if (previous.revision != expected_revision)
            return Error::from_errno(EAGAIN);
        if (previous.revision >= static_cast<u64>(NumericLimits<i64>::max()))
            return Error::from_errno(EOVERFLOW);
        auto stats = TRY(statistics(*m_database, m_statements->statistics, key));
        auto old_size = previous.size;
        if (stats.partition_bytes < old_size || stats.session_bytes < old_size)
            return Error::from_errno(EINVAL);
        auto partition_without_old = stats.partition_bytes - old_size;
        auto session_without_old = stats.session_bytes - old_size;
        if (bytes.size() > m_limits.partition_bytes || bytes.size() > m_limits.session_bytes
            || partition_without_old > m_limits.partition_bytes - bytes.size() || session_without_old > m_limits.session_bytes - bytes.size())
            return Error::from_errno(ENOSPC);
        revision = previous.revision + 1;
        return m_database->try_execute_bound_statement(m_statements->update, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("id"sv, static_cast<i64>(id)));
            TRY(bind("revision"sv, static_cast<i64>(revision)));
            TRY(bind("snapshot"sv, bytes));
            return bind_time(bind, now());
        });
    }));
    return revision;
}

ErrorOr<void> CacheStorageStore::release(String const& key, u64 id)
{
    return m_database->transaction([&]() -> ErrorOr<void> {
        (void)TRY(metadata(key, id));
        auto bind_id = [&](auto& bind) -> ErrorOr<void> { return bind("id"sv, static_cast<i64>(id)); };
        TRY(m_database->try_execute_bound_statement(m_statements->release, bind_id));
        return m_database->try_execute_bound_statement(m_statements->remove_unleased, bind_id);
    });
}

ErrorOr<Vector<u64>> CacheStorageStore::clear_accessed_since(UnixDateTime since)
{
    Vector<u64> invalidated;
    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        auto bind_since = [&](auto& bind) -> ErrorOr<void> { return bind_time(bind, since); };
        invalidated = TRY(m_database->try_collect_bound_statement<u64>(m_statements->find_accessed_since, m_limits.identities,
            bind_since, [](Database::ResultRow& row) -> ErrorOr<u64> { return row.read_integer<u64>("id"sv); }));
        return m_database->try_execute_bound_statement(m_statements->clear_accessed_since, bind_since);
    }));
    return invalidated;
}

ErrorOr<Requests::CacheSizes> CacheStorageStore::estimate_accessed_since(UnixDateTime since) const
{
    return m_database->try_execute_bound_statement_one<Requests::CacheSizes>(m_statements->estimate_accessed_since, [&](auto& bind) -> ErrorOr<void> { return bind_time(bind, since); }, [](Database::ResultRow& row) -> ErrorOr<Requests::CacheSizes> { return Requests::CacheSizes {
                                                                                                                                                                                                                                                             .since_requested_time = TRY(row.read_integer<u64>("since_requested_time"sv)),
                                                                                                                                                                                                                                                             .total = TRY(row.read_integer<u64>("total"sv)),
                                                                                                                                                                                                                                                         }; });
}

}
