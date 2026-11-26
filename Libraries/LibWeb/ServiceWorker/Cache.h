/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGC/Function.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/CacheStorage.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocol.h>

namespace Web::ServiceWorker {

using CacheQueryOptions = Bindings::CacheQueryOptions;
using MultiCacheQueryOptions = Bindings::MultiCacheQueryOptions;

class CacheBodyStaging;

// https://w3c.github.io/ServiceWorker/#dfn-cache-batch-operation
struct CacheBatchOperation : public GC::Cell {
    GC_CELL(CacheBatchOperation, GC::Cell);
    GC_DECLARE_ALLOCATOR(CacheBatchOperation);

    enum class Type : u8 {
        Delete,
        Put,
    };

    CacheBatchOperation(Type type, NonnullRefPtr<Fetch::Infrastructure::Request> request, GC::Ptr<Fetch::Infrastructure::ResponseView> response = {}, CacheQueryOptions options = {})
        : type(type)
        , request(request)
        , response(response)
        , options(options)
    {
    }

    virtual void visit_edges(Visitor&) override;

    Type type;
    NonnullRefPtr<Fetch::Infrastructure::Request> request;
    GC::Ptr<Fetch::Infrastructure::ResponseView> response;
    CacheQueryOptions options;
};

// https://w3c.github.io/ServiceWorker/#cache-interface
class Cache : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Cache, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Cache);

public:
    void match(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void match_all(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void add(JS::Realm&, Fetch::RequestInfo, GC::Ref<WebIDL::Promise>);
    void add_all(JS::Realm&, ReadonlySpan<Fetch::RequestInfo>, GC::Ref<WebIDL::Promise>);
    void put(JS::Realm&, Fetch::RequestInfo, GC::Ref<Fetch::Response>, GC::Ref<WebIDL::Promise>);
    void delete_(JS::Realm&, Fetch::RequestInfo, CacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void keys(JS::Realm&, Optional<Fetch::RequestInfo>, CacheQueryOptions, GC::Ref<WebIDL::Promise>);

private:
    friend class CacheStorage;

    explicit Cache(GC::Ref<RequestResponseList>);
    Cache(HTML::EnvironmentSettingsObject&, CacheStorageIPC::CacheId);
    void release_remote_lease();

    virtual void finalize() override;

    virtual void visit_edges(GC::Cell::Visitor&) override;

    enum class CloneCache {
        No,
        Yes,
    };
    enum class BatchResult {
        Undefined,
        Removed,
    };
    using BatchOperations = GC::HeapVector<GC::Ref<CacheBatchOperation>>;
    using SnapshotCallback = GC::Function<void(GC::Ref<RequestResponseList>, CacheStorageIPC::Revision)>;

    void read_snapshot(GC::Ref<WebIDL::Promise>, GC::Ref<SnapshotCallback>, RefPtr<CacheBodyStaging> = {});
    void run_cache_batch(JS::Realm&, GC::Ref<BatchOperations>, GC::Ref<WebIDL::Promise>, BatchResult, RefPtr<CacheBodyStaging> = {}, unsigned retries_remaining = 3);
    static void settle_cache_batch(JS::Realm&, GC::Ref<WebIDL::Promise>, BatchResult, WebIDL::ExceptionOr<bool>);
    static GC::Ref<RequestResponseList> query_cache(JS::Realm&, GC::Ref<RequestResponseList>, NonnullRefPtr<Fetch::Infrastructure::Request> request_query, CacheQueryOptions, CloneCache = CloneCache::Yes);
    static WebIDL::ExceptionOr<bool> batch_cache_operations(JS::Realm&, GC::Ref<RequestResponseList>, GC::Ref<BatchOperations>);

    GC::Ptr<RequestResponseList> m_request_response_list;
    GC::Weak<HTML::EnvironmentSettingsObject> m_owner;
    Optional<CacheStorageIPC::CacheId> m_remote_id;
};

}
