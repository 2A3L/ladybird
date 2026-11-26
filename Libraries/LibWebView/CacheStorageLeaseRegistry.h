/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocol.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>

namespace WebView {

// The endpoint identity comes from the receiving connection, never from a message.
struct CacheStorageOwner {
    enum class Kind : u8 { Page,
        Worker };
    Kind kind;
    void const* identity;

    bool operator==(CacheStorageOwner const&) const = default;
};

class WEBVIEW_API CacheStorageLeaseRegistry {
public:
    static CacheStorageLeaseRegistry& the();

    // Release is authorized by ownership alone and ignores the partition key, since the document may no longer be
    // active when it returns its lease. Every other request requires a key.
    Web::ServiceWorker::CacheStorageIPC::Reply dispatch(CacheStorageOwner, BrowsingSession&, CanonicalEnvironmentSettingsObject const&, Optional<String> const& partition_key, Web::ServiceWorker::CacheStorageIPC::Request const&);
    void remove_owner(CacheStorageOwner);
    void remove_environment(CanonicalEnvironmentSettingsObject const&);
    // One cache is stored as a snapshot: clearing by time removes the whole cache, not individual entries.
    ErrorOr<void> clear_accessed_since(BrowsingSession&, UnixDateTime since);

private:
    void remove_invalidated(BrowsingSession&, ReadonlySpan<u64> ids);

    struct Lease {
        CacheStorageOwner owner;
        WeakPtr<BrowsingSession> session;
        CanonicalEnvironmentSettingsObject const* environment;
        String partition_key;
        u64 id;
        bool revoked { false };
    };

    Optional<size_t> find_lease(CacheStorageOwner, BrowsingSession const&, CanonicalEnvironmentSettingsObject const&, Optional<String> const&, u64) const;
    void retry_revoked();
    Vector<Lease> m_leases;
};

}
