/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#cachestorage-interface
class CacheStorage : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CacheStorage, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CacheStorage);

public:
    void match(JS::Realm&, Fetch::RequestInfo, MultiCacheQueryOptions, GC::Ref<WebIDL::Promise>);
    void has(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void open(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void delete_(JS::Realm&, Utf16String const& cache_name, GC::Ref<WebIDL::Promise>);
    void keys(JS::Realm&, GC::Ref<WebIDL::Promise>);
    bool contains_cache(Utf16String const& cache_name);
    bool delete_cache(Utf16String const& cache_name);
    Vector<Utf16String> cache_keys();
    GC::Ref<RequestResponseList> get_or_create_cache(Utf16String const& cache_name);

private:
    explicit CacheStorage(HTML::EnvironmentSettingsObject&);

    bool uses_hub() const;
    void match_in_hub(JS::Realm&, Fetch::RequestInfo, MultiCacheQueryOptions, GC::Ref<WebIDL::Promise>);
    static void match_in_leased_caches(JS::Realm&, GC::Ref<GC::HeapVector<GC::Ref<Cache>>>, Fetch::RequestInfo, MultiCacheQueryOptions, GC::Ref<WebIDL::Promise>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    NameToCacheMap& relevant_name_to_cache_map();

    NameToCacheMap m_relevant_name_to_cache_map;
    GC::Weak<HTML::EnvironmentSettingsObject> m_owner;
};

}
