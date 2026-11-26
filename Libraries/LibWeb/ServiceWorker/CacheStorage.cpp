/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/CacheStorage.h>
#include <LibWeb/ServiceWorker/CacheStorageConnection.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(CacheStorage);

namespace IPC = CacheStorageIPC;

static Atomic<bool> s_cache_storage_hub_is_enabled { false };

void set_cache_storage_hub_enabled(bool enabled)
{
    s_cache_storage_hub_is_enabled.store(enabled, AK::MemoryOrder::memory_order_relaxed);
}

bool cache_storage_hub_is_enabled()
{
    return s_cache_storage_hub_is_enabled.load(AK::MemoryOrder::memory_order_relaxed);
}

bool cache_storage_environment_is_live(GC::Ptr<HTML::EnvironmentSettingsObject> settings)
{
    if (!settings || settings->discarded())
        return false;
    auto& global = settings->global_object();
    if (auto* window = HTML::window_from_global_object(global))
        return !window->associated_document().has_been_destroyed();
    if (auto* worker = Bindings::worker_global_scope_from_global_object(global))
        return !worker->is_closing();
    return true;
}

GC::Ptr<PageClient> cache_storage_page_client(GC::Ptr<HTML::EnvironmentSettingsObject> owner)
{
    if (!cache_storage_environment_is_live(owner))
        return {};

    auto& global = owner->global_object();
    if (auto* window = HTML::window_from_global_object(global))
        return &window->page().client();
    if (auto* worker = Bindings::worker_global_scope_from_global_object(global); worker && worker->page())
        return &worker->page()->client();
    return {};
}

Optional<IPC::StorageError> send_cache_storage_request(GC::Ptr<HTML::EnvironmentSettingsObject> owner, IPC::Request request, GC::Ptr<CacheStorageReplyCallback> callback)
{
    if (auto client = cache_storage_page_client(owner))
        return client->request_cache_storage(owner->id, move(request), callback);
    return IPC::StorageError::Unavailable;
}

bool cache_storage_promise_is_live(WebIDL::Promise const& promise)
{
    return cache_storage_environment_is_live(&HTML::relevant_settings_object(promise.promise()));
}

void reject_cache_storage_request(WebIDL::Promise& promise, IPC::StorageError error)
{
    switch (error) {
    case IPC::StorageError::Denied:
        WebIDL::reject_promise(promise, WebIDL::SecurityError::create("Cache Storage is not available for this environment"_utf16));
        return;
    case IPC::StorageError::QuotaExceeded:
        WebIDL::reject_promise(promise, WebIDL::QuotaExceededError::create("Cache Storage quota exceeded"_utf16));
        return;
    case IPC::StorageError::Missing:
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("This cache is no longer available"_utf16));
        return;
    case IPC::StorageError::StaleRevision:
    case IPC::StorageError::Unavailable:
        WebIDL::reject_promise(promise, WebIDL::AbortError::create("Could not complete the Cache Storage operation"_utf16));
        return;
    }
    VERIFY_NOT_REACHED();
}

void queue_cache_storage_rejection(WebIDL::Promise& promise, IPC::StorageError error)
{
    auto& settings = HTML::relevant_settings_object(promise.promise());

    // A worker's promises cannot reach another agent, so nothing can observe them once the worker is closing.
    if (auto* worker = Bindings::worker_global_scope_from_global_object(settings.global_object()); worker && worker->is_closing())
        return;

    // Another realm can still hold the promise of an owner that is gone, so it must settle. A global task of a destroyed
    // document would never run, so this task has no document, like those that settle renderer-local caches.
    HTML::queue_a_task(HTML::Task::Source::DOMManipulation, settings.responsible_event_loop(), {},
        GC::create_function(GC::Heap::the(), GC::captures(promise, error), [](auto& promise, auto& error) {
            HTML::TemporaryExecutionContext context { WebIDL::promise_realm(promise), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            // An operation whose owner went away was cut short.
            reject_cache_storage_request(promise, cache_storage_promise_is_live(promise) ? error : IPC::StorageError::Unavailable);
        }));
}

static void release_cache_storage_reply_leases(GC::Ptr<HTML::EnvironmentSettingsObject> owner, IPC::Reply const& reply)
{
    if (auto const* opened = reply.get_pointer<IPC::Opened>()) {
        (void)send_cache_storage_request(owner, IPC::Release { opened->id });
    } else if (auto const* found = reply.get_pointer<IPC::Found>(); found && found->id.has_value()) {
        (void)send_cache_storage_request(owner, IPC::Release { *found->id });
    } else if (auto const* cache_ids = reply.get_pointer<IPC::CacheIds>()) {
        for (auto id : cache_ids->ids)
            (void)send_cache_storage_request(owner, IPC::Release { id });
    }
}

template<typename Response, typename Success>
static void perform_cache_storage_request(GC::Weak<HTML::EnvironmentSettingsObject> owner, GC::Ref<WebIDL::Promise> promise, IPC::Request request, GC::Ref<Success> on_success)
{
    if (!cache_storage_promise_is_live(promise)) {
        queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
        return;
    }

    auto callback = GC::create_function(GC::Heap::the(), GC::captures(owner, promise, on_success), [](auto& owner, auto& promise, auto& on_success, IPC::Reply reply) {
        auto settings = owner.ptr();
        if (cache_storage_environment_is_live(settings) && cache_storage_promise_is_live(promise)) {
            if (auto* response = reply.get_pointer<Response>()) {
                HTML::TemporaryExecutionContext context { WebIDL::promise_realm(promise), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                on_success->function()(move(*response));
                return;
            }
        }

        // If the owner itself is gone, the Hub revokes its leases when it tears down the environment.
        release_cache_storage_reply_leases(settings, reply);
        auto* error = reply.get_pointer<IPC::StorageError>();
        queue_cache_storage_rejection(promise, error ? *error : IPC::StorageError::Unavailable);
    });
    if (auto error = send_cache_storage_request(owner.ptr(), move(request), callback); error.has_value())
        queue_cache_storage_rejection(promise, *error);
}

CacheStorage::CacheStorage(HTML::EnvironmentSettingsObject& owner)
    : m_owner(owner)
{
}

bool CacheStorage::uses_hub() const
{
    return cache_storage_hub_is_enabled() && cache_storage_page_client(m_owner.ptr());
}

void CacheStorage::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_relevant_name_to_cache_map);
}

// https://w3c.github.io/ServiceWorker/#cache-storage-match
void CacheStorage::match(JS::Realm& realm, Fetch::RequestInfo request, MultiCacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    if (uses_hub()) {
        match_in_hub(realm, move(request), move(options), promise);
        return;
    }

    // 1. If options["cacheName"] exists, then:
    if (options.cache_name.has_value()) {
        // 1. Return a new promise promise and run the following substeps in parallel:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, move(request), move(options)), [](auto& self, auto& realm, auto& promise, auto& request, auto& options) {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. For each cacheName → cache of the relevant name to cache map:
            //     1. If options["cacheName"] matches cacheName, then:
            if (auto result = self.relevant_name_to_cache_map().get(*options.cache_name); result.has_value()) {
                // 1. Resolve promise with the result of running the algorithm specified in match(request, options)
                //    method of Cache interface with request and options (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = GC::Heap::the().allocate<Cache>(result.release_value());
                auto match_promise = WebIDL::create_promise(realm);
                cache->match(realm, move(request), move(options), match_promise);
                WebIDL::resolve_promise(promise, match_promise->promise());

                // 2. Abort these steps.
                return;
            }

            // 2. Resolve promise with undefined.
            WebIDL::resolve_promise(promise);
        }));
    }
    // 2. Else:
    else {
        // 1. Let promise be a promise resolved with undefined.
        auto internal_promise = WebIDL::create_resolved_promise(realm, JS::js_undefined());

        // 2. For each cacheName → cache of the relevant name to cache map:
        for (auto const& [cache_name, cache_list] : relevant_name_to_cache_map()) {
            // 1. Set promise to the result of reacting to itself with a fulfillment handler that, when called with
            //    argument response, performs the following substeps:
            internal_promise = WebIDL::upon_fulfillment(internal_promise, GC::create_function(GC::Heap::the(), GC::captures(realm, cache_list, request, options), [](auto& realm, auto& cache_list, auto& request, auto& options, JS::Value response) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If response is not undefined, return response.
                if (!response.is_undefined())
                    return response;

                // 2. Return the result of running the algorithm specified in match(request, options) method of Cache
                //    interface with request and options as the arguments (providing cache as thisArgument to the
                //    [[Call]] internal method of match(request, options).)
                auto cache = GC::Heap::the().allocate<Cache>(cache_list);
                auto match_promise = WebIDL::create_promise(realm);
                cache->match(realm, move(request), move(options), match_promise);
                return match_promise->promise();
            }));
        }

        WebIDL::react_to_promise(internal_promise,
            GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                WebIDL::resolve_promise(promise, value);
                return JS::js_undefined();
            }),
            GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                WebIDL::reject_promise(promise, reason);
                return JS::js_undefined();
            }));
    }
}

void CacheStorage::match_in_hub(JS::Realm& realm, Fetch::RequestInfo request, MultiCacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    if (options.cache_name.has_value()) {
        auto name = IPC::Name { *options.cache_name };
        auto on_found = GC::create_function(GC::Heap::the(), GC::captures(m_owner, realm, promise, move(request), move(options)), [](auto& owner, auto& realm, auto& promise, auto& request, auto& options, IPC::Found found) {
            if (!found.id.has_value()) {
                WebIDL::resolve_promise(promise);
                return;
            }
            auto caches = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Cache>>>();
            caches->elements().append(GC::Heap::the().allocate<Cache>(*owner.ptr(), *found.id));
            match_in_leased_caches(realm, caches, move(request), move(options), promise);
        });
        perform_cache_storage_request<IPC::Found>(m_owner, promise, IPC::Lookup { move(name) }, on_found);
        return;
    }

    auto on_enumerated = GC::create_function(GC::Heap::the(), GC::captures(m_owner, realm, promise, move(request), move(options)), [](auto& owner, auto& realm, auto& promise, auto& request, auto& options, IPC::CacheIds ids) {
        auto caches = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Cache>>>();
        for (auto id : ids.ids)
            caches->elements().append(GC::Heap::the().allocate<Cache>(*owner.ptr(), id));
        match_in_leased_caches(realm, caches, move(request), move(options), promise);
    });
    perform_cache_storage_request<IPC::CacheIds>(m_owner, promise, IPC::Enumerate {}, on_enumerated);
}

void CacheStorage::match_in_leased_caches(JS::Realm& realm, GC::Ref<GC::HeapVector<GC::Ref<Cache>>> caches, Fetch::RequestInfo request, MultiCacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    auto result = WebIDL::create_resolved_promise(realm, JS::js_undefined());
    for (auto cache : caches->elements()) {
        result = WebIDL::upon_fulfillment(result, GC::create_function(GC::Heap::the(), GC::captures(realm, cache, request, options), [](auto& realm, auto& cache, auto& request, auto& options, JS::Value response) -> WebIDL::ExceptionOr<JS::Value> {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            if (!response.is_undefined())
                return response;

            auto match_promise = WebIDL::create_promise(realm);
            cache->match(realm, move(request), move(options), match_promise);
            return match_promise->promise();
        }));
    }

    WebIDL::react_to_promise(result,
        GC::create_function(GC::Heap::the(), GC::captures(caches, promise), [](auto& caches, auto& promise, JS::Value result) -> WebIDL::ExceptionOr<JS::Value> {
            for (auto cache : caches->elements())
                cache->release_remote_lease();
            WebIDL::resolve_promise(promise, result);
            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(caches, promise), [](auto& caches, auto& promise, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            for (auto cache : caches->elements())
                cache->release_remote_lease();
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-has
void CacheStorage::has(JS::Realm& realm, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    if (uses_hub()) {
        auto callback = GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, IPC::Exists result) {
            WebIDL::resolve_promise(promise, JS::Value { result.exists });
        });
        perform_cache_storage_request<IPC::Exists>(m_owner, promise, IPC::Has { { cache_name } }, callback);
        return;
    }

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, cache_name), [](auto& self, auto& realm, auto& promise, auto& cache_name) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. For each key -> value of the relevant name to cache map:
        //     1. If cacheName matches key, resolve promise with true and abort these steps.
        // 2. Resolve promise with false.
        WebIDL::resolve_promise(promise, JS::Value { self.contains_cache(cache_name) });
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-open
void CacheStorage::open(JS::Realm& realm, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    if (uses_hub()) {
        auto callback = GC::create_function(GC::Heap::the(), GC::captures(m_owner, realm, promise), [](auto& owner, auto& realm, auto& promise, IPC::Opened result) {
            auto cache = GC::Heap::the().allocate<Cache>(*owner.ptr(), result.id);
            WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, cache));
        });
        perform_cache_storage_request<IPC::Opened>(m_owner, promise, IPC::Open { { cache_name } }, callback);
        return;
    }

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, cache_name), [](auto& self, auto& realm, auto& promise, auto& cache_name) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. If cacheName matches an existing cache, resolve promise with a new Cache object that represents it.
        // 2. Otherwise, create a new request response list and set the relevant name to cache map[cacheName] to it.
        //    If this cache write operation failed due to exceeding the granted quota limit, reject promise with a
        //    QuotaExceededError and abort these steps.
        // FIXME: Handle cache quotas in CacheStorage::get_or_create_cache().
        auto cache = self.get_or_create_cache(cache_name);

        // 4. Resolve promise with a new Cache object that represents cache.
        auto cache_object = GC::Heap::the().allocate<Cache>(cache);
        WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, cache_object));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-delete
void CacheStorage::delete_(JS::Realm& realm, Utf16String const& cache_name, GC::Ref<WebIDL::Promise> promise)
{
    if (uses_hub()) {
        auto callback = GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, IPC::Deleted result) {
            WebIDL::resolve_promise(promise, JS::Value { result.deleted });
        });
        perform_cache_storage_request<IPC::Deleted>(m_owner, promise, IPC::Delete { { cache_name } }, callback);
        return;
    }

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, cache_name), [](auto& self, auto& realm, auto& promise, auto& cache_name) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. If cacheName does not match an entry, resolve promise with false and abort these steps.
        // 2. Otherwise, remove the relevant name to cache map[cacheName] and resolve promise with true.
        auto const removed_cache = self.delete_cache(cache_name);
        WebIDL::resolve_promise(promise, JS::Value { removed_cache });

        // Note: After this step, the existing DOM objects (i.e. the currently referenced Cache, Request, and Response
        //       objects) should remain functional.
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-storage-keys
void CacheStorage::keys(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise)
{
    if (uses_hub()) {
        auto callback = GC::create_function(GC::Heap::the(), GC::captures(realm, promise), [](auto& realm, auto& promise, IPC::Names result) {
            WebIDL::resolve_promise(promise, JS::Array::create_from<IPC::Name>(realm, result.names, [&](auto const& name) {
                return JS::PrimitiveString::create(realm.vm(), name.value);
            }));
        });
        perform_cache_storage_request<IPC::Names>(m_owner, promise, IPC::List {}, callback);
        return;
    }

    // 2. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise), [](auto& self, auto& realm, auto& promise) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let cacheKeys be the result of getting the keys of the relevant name to cache map.
        auto cache_keys = self.cache_keys();

        // Note: The items in the result ordered set are in the order that their corresponding entry was added to the
        //       name to cache map.

        // 2. Resolve promise with cacheKeys.
        WebIDL::resolve_promise(promise, JS::Array::create_from<Utf16String>(realm, cache_keys, [&](Utf16String const& cache_key) {
            return JS::PrimitiveString::create(realm.vm(), cache_key);
        }));
    }));
}

bool CacheStorage::contains_cache(Utf16String const& cache_name)
{
    return relevant_name_to_cache_map().contains(cache_name);
}

bool CacheStorage::delete_cache(Utf16String const& cache_name)
{
    if (!contains_cache(cache_name))
        return false;

    relevant_name_to_cache_map().remove(cache_name);
    return true;
}

GC::Ref<RequestResponseList> CacheStorage::get_or_create_cache(Utf16String const& cache_name)
{
    if (auto cache = relevant_name_to_cache_map().get(cache_name); cache.has_value())
        return cache.release_value();

    auto cache = GC::Heap::the().allocate<RequestResponseList>();
    relevant_name_to_cache_map().set(cache_name, cache);
    return cache;
}

Vector<Utf16String> CacheStorage::cache_keys()
{
    return relevant_name_to_cache_map().keys();
}

// https://w3c.github.io/ServiceWorker/#relevant-name-to-cache-map
NameToCacheMap& CacheStorage::relevant_name_to_cache_map()
{
    // The relevant name to cache map for a CacheStorage object is the name to cache map associated with the result of
    // running obtain a local storage bottle map with the object’s relevant settings object and "caches".

    // FIXME: We don't yet have a way to serialize request/response pairs for storage in the UI process. For now, we
    //        use an ephemeral name to cache map.
    return m_relevant_name_to_cache_map;
}

}
