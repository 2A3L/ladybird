/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Fetch/Fetching/Checks.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/Cache.h>
#include <LibWeb/ServiceWorker/CacheBodyReader.h>
#include <LibWeb/ServiceWorker/CacheResponsePolicy.h>
#include <LibWeb/ServiceWorker/CacheSnapshot.h>
#include <LibWeb/ServiceWorker/CacheStorageConnection.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Cache);
GC_DEFINE_ALLOCATOR(CacheBatchOperation);

namespace IPC = CacheStorageIPC;

bool cached_response_is_blocked_by_cross_origin_policy(URL::Origin const& origin, HTML::EmbedderPolicy const& policy, Fetch::Infrastructure::Response const& response)
{
    if (response.type() != Fetch::Infrastructure::Response::Type::Opaque)
        return false;

    auto internal_response = response.unsafe_response();
    return !internal_response->url().has_value()
        || Fetch::Fetching::cross_origin_resource_policy_check(origin, policy, *internal_response) == Fetch::Infrastructure::RequestOrResponseBlocking::Blocked;
}

static void resolve_cache_response_list(JS::Realm& realm, WebIDL::Promise& promise, GC::Ref<GC::HeapVector<GC::Ref<Fetch::Infrastructure::ResponseView>>> responses)
{
    auto response_list = GC::Heap::the().allocate<GC::HeapVector<JS::Value>>();

    for (auto response : responses->elements())
        response_list->elements().append(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, Fetch::Response::create(response, Fetch::Headers::Guard::Immutable)));

    WebIDL::resolve_promise(promise, JS::Array::create_from(realm, response_list->elements()));
}

static void resolve_cache_request_list(JS::Realm& realm, WebIDL::Promise& promise, Vector<NonnullRefPtr<Fetch::Infrastructure::Request>> const& requests)
{
    auto request_list = GC::Heap::the().allocate<GC::HeapVector<JS::Value>>();

    for (auto const& request : requests)
        request_list->elements().append(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, Fetch::Request::create(request, Fetch::Headers::Guard::Immutable, DOM::AbortSignal::create())));

    WebIDL::resolve_promise(promise, JS::Array::create_from(realm, request_list->elements()));
}

enum class CacheResponseSource {
    Put,
    Fetch,
};

using CacheFetchControllers = GC::HeapVector<GC::Ref<Fetch::Infrastructure::FetchController>>;

static constexpr size_t maximum_cache_fetches_per_operation = 8;
static constexpr size_t maximum_cache_fetches_per_process = 32;
static Atomic<size_t> s_reserved_cache_fetch_slots { 0 };

class CacheBodyStaging final : public RefCounted<CacheBodyStaging> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    static NonnullRefPtr<CacheBodyStaging> create()
    {
        return adopt_ref(*new CacheBodyStaging);
    }

    ~CacheBodyStaging()
    {
        release_fetch_slots();
        release_staged_bodies();
    }

    NonnullRefPtr<CacheBodyBudget> const& budget() const { return m_budget; }

    size_t reserve_fetch_slots(size_t request_count)
    {
        VERIFY(!m_fetch_slots);
        VERIFY(request_count);
        auto reserved = s_reserved_cache_fetch_slots.load(AK::MemoryOrder::memory_order_relaxed);
        while (reserved < maximum_cache_fetches_per_process) {
            auto slots = min(min(request_count, maximum_cache_fetches_per_operation), maximum_cache_fetches_per_process - reserved);
            if (s_reserved_cache_fetch_slots.compare_exchange_strong(reserved, reserved + slots, AK::MemoryOrder::memory_order_relaxed)) {
                m_fetch_slots = slots;
                return slots;
            }
        }
        return 0;
    }

    void release_fetch_slots()
    {
        auto slots = exchange(m_fetch_slots, 0);
        if (slots) {
            auto previous = s_reserved_cache_fetch_slots.fetch_sub(slots, AK::MemoryOrder::memory_order_relaxed);
            VERIFY(previous >= slots);
        }
    }

    bool observe_preparation(GC::Ptr<HTML::EnvironmentSettingsObject> owner, GC::Ref<WebIDL::Promise> promise, GC::Ptr<CacheFetchControllers> fetch_controllers = {})
    {
        auto cancel = GC::create_function(GC::Heap::the(), GC::captures(NonnullRefPtr { *this }, promise, fetch_controllers), [](auto& staging, auto& promise, auto& fetch_controllers) {
            if (!staging->m_owner_preparation.has_value())
                return;

            if (fetch_controllers) {
                for (auto controller : fetch_controllers->elements())
                    controller->terminate();
            }
            staging->discard();
            queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
        });

        auto register_preparation = [&](GC::Ptr<HTML::EnvironmentSettingsObject> settings) -> Optional<PreparationRegistration> {
            auto client = cache_storage_page_client(settings);
            if (!client)
                return {};
            auto id = client->register_cache_storage_preparation(settings->id, cancel);
            if (id.is_error())
                return {};
            return PreparationRegistration { *client, id.release_value() };
        };

        m_owner_preparation = register_preparation(owner);
        if (!m_owner_preparation.has_value())
            return false;

        // The body and its promise can belong to a different environment from the cache owner.
        auto& caller = HTML::relevant_settings_object(promise->promise());
        if (&caller != owner.ptr()) {
            m_caller_preparation = register_preparation(&caller);
            if (!m_caller_preparation.has_value()) {
                finish_preparation();
                return false;
            }
        }
        return true;
    }

    void finish_preparation()
    {
        for (auto* registration : { &m_owner_preparation, &m_caller_preparation }) {
            if (registration->has_value()) {
                auto [client, id] = registration->release_value();
                if (auto live_client = client.ptr())
                    live_client->unregister_cache_storage_preparation(id);
            }
        }
    }

    void stage(NonnullRefPtr<Fetch::Infrastructure::Response> record, ByteBuffer bytes)
    {
        VERIFY(!m_budget->is_discarded());
        m_staged_bytes += bytes.size();
        m_bodies.append({ record, *record->body() });
        record->set_body(Fetch::Infrastructure::ResponseBody::from_bytes(Core::ImmutableBytes::adopt(move(bytes))));
    }

    void discard()
    {
        finish_preparation();
        m_budget->discard();
        release_fetch_slots();
        release_staged_bodies();
    }

private:
    void release_staged_bodies()
    {
        // Only the private records own staged bytes. Their stream views still refer to the source streams.
        for (auto& body : m_bodies)
            body.record->set_body(move(body.original));
        m_bodies.clear();
        m_budget->release(exchange(m_staged_bytes, 0));
    }

    struct Body {
        NonnullRefPtr<Fetch::Infrastructure::Response> record;
        Fetch::Infrastructure::ResponseBody original;
    };

    struct PreparationRegistration {
        GC::Weak<PageClient> client;
        u64 id;
    };

    NonnullRefPtr<CacheBodyBudget> m_budget { CacheBodyBudget::create(IPC::maximum_snapshot_size) };
    Optional<PreparationRegistration> m_owner_preparation;
    Optional<PreparationRegistration> m_caller_preparation;
    Vector<Body> m_bodies;
    size_t m_fetch_slots { 0 };
    size_t m_staged_bytes { 0 };
};

static void materialize_cache_response(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::ResponseView> response, GC::Ref<WebIDL::Promise> promise, RefPtr<CacheBodyStaging> staging, CacheResponseSource source)
{
    auto body = response->body();
    if (!body && staging && response->record()->unsafe_response()->body().has_value()) {
        auto private_body = response->unsafe_response()->body();
        if (auto stream = private_body->stream(); stream->is_disturbed() || stream->is_locked()) {
            WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Could not copy the cached response body"sv));
            return;
        }
        // A filtered response can have a private body. Consume its clone while preserving the original filter and stream.
        response = response->clone(realm);
        body = response->unsafe_response()->body();
    }
    if (!body) {
        WebIDL::resolve_promise(promise, response->clone(realm));
        return;
    }

    auto reader = body->stream()->get_a_reader();
    if (reader.is_exception()) {
        WebIDL::reject_promise_with_exception(promise, reader.release_error());
        return;
    }

    auto record = response->record()->clone();
    if (staging) {
        auto success = GC::create_function(GC::Heap::the(), GC::captures(promise, body, record, staging), [](auto& promise, auto& body, auto& record, auto& staging, ByteBuffer bytes) {
            staging->stage(record->unsafe_response(), move(bytes));
            WebIDL::resolve_promise(promise, Fetch::Infrastructure::ResponseView::create(move(record), body));
        });
        auto failure = GC::create_function(GC::Heap::the(), GC::captures(realm, promise, response, source), [](auto& realm, auto& promise, auto& response, auto& source, JS::Value error, CacheBodyFailure failure) {
            if (source == CacheResponseSource::Put || failure == CacheBodyFailure::Quota) {
                WebIDL::reject_promise(promise, error);
            } else if (response->aborted()) {
                WebIDL::reject_promise(promise, WebIDL::AbortError::create("Fetch request was aborted"_utf16));
            } else {
                WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Fetch response body failed"sv));
            }
        });
        read_cache_body(realm, *reader.value(), staging->budget(), success, failure);
        return;
    }

    auto success = GC::create_function(GC::Heap::the(), GC::captures(realm, promise, move(record)), [](auto& realm, auto& promise, auto& record, ByteBuffer bytes) {
        record->unsafe_response()->set_body(Fetch::Infrastructure::ResponseBody::from_bytes(Core::ImmutableBytes::adopt(move(bytes))));
        WebIDL::resolve_promise(promise, Fetch::Infrastructure::ResponseView::create(realm, move(record)));
    });
    auto failure = GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value error) {
        WebIDL::reject_promise(promise, error);
    });
    reader.value()->read_all_bytes(success, failure);
}

Cache::Cache(GC::Ref<RequestResponseList> request_response_list)
    : m_request_response_list(request_response_list)
{
}

Cache::Cache(HTML::EnvironmentSettingsObject& owner, IPC::CacheId id)
    : m_owner(owner)
    , m_remote_id(id)
{
}

void Cache::release_remote_lease()
{
    if (m_remote_id.has_value())
        (void)send_cache_storage_request(m_owner.ptr(), IPC::Release { m_remote_id.release_value() });
}

void Cache::finalize()
{
    release_remote_lease();
    Base::finalize();
}

void Cache::read_snapshot(GC::Ref<WebIDL::Promise> promise, GC::Ref<SnapshotCallback> callback, RefPtr<CacheBodyStaging> staging)
{
    if (!m_remote_id.has_value()) {
        callback->function()(*m_request_response_list, IPC::Revision { 0 });
        return;
    }
    if (!cache_storage_promise_is_live(promise)) {
        if (staging)
            staging->discard();
        queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
        return;
    }

    auto reply_callback = GC::create_function(GC::Heap::the(), GC::captures(*this, promise, callback, staging), [](auto& self, auto& promise, auto& callback, auto& staging, IPC::Reply reply) {
        ArmedScopeGuard discard_on_failure = [&] {
            if (staging)
                staging->discard();
        };
        if (!cache_storage_promise_is_live(promise)) {
            queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
            return;
        }
        auto owner = self.m_owner.ptr();
        if (!cache_storage_environment_is_live(owner)) {
            queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
            return;
        }
        auto* loaded = reply.get_pointer<IPC::Loaded>();
        if (!loaded) {
            auto* error = reply.get_pointer<IPC::StorageError>();
            queue_cache_storage_rejection(promise, error ? *error : IPC::StorageError::Unavailable);
            return;
        }

        auto const owner_origin = owner->origin();
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // A newly created cache has no serialized snapshot yet.
        auto decoded = loaded->revision.value() == 0 && loaded->snapshot.bytes.is_empty()
            ? ErrorOr<GC::Ref<RequestResponseList>> { GC::Heap::the().allocate<RequestResponseList>() }
            : decode_cache_snapshot(realm, loaded->snapshot.bytes.bytes(), owner_origin, IPC::maximum_snapshot_size);
        if (decoded.is_error()) {
            if (staging)
                staging->discard();
            discard_on_failure.disarm();
            WebIDL::reject_promise(promise, WebIDL::OperationError::create("Could not read the Cache Storage snapshot"_utf16));
            return;
        }
        discard_on_failure.disarm();
        callback->function()(decoded.release_value(), loaded->revision);
    });
    if (auto error = send_cache_storage_request(m_owner.ptr(), IPC::Read { *m_remote_id }, reply_callback); error.has_value()) {
        if (staging)
            staging->discard();
        queue_cache_storage_rejection(promise, *error);
    }
}

void Cache::settle_cache_batch(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise, BatchResult result_kind, WebIDL::ExceptionOr<bool> result)
{
    // The operation is complete, so its promise settles with its result even once the owner is gone.
    HTML::queue_a_task(
        HTML::Task::Source::DOMManipulation,
        HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
        {},
        GC::create_function(GC::Heap::the(), GC::captures(realm, promise, result_kind, move(result)), [](auto& realm, auto& promise, auto& result_kind, auto& result) {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            if (result.is_error())
                WebIDL::reject_promise_with_exception(promise, result.release_error());
            else if (result_kind == BatchResult::Removed)
                WebIDL::resolve_promise(promise, JS::Value { result.value() });
            else
                WebIDL::resolve_promise(promise);
        }));
}

void Cache::run_cache_batch(JS::Realm& realm, GC::Ref<BatchOperations> operations, GC::Ref<WebIDL::Promise> promise, BatchResult result_kind, RefPtr<CacheBodyStaging> staging, unsigned retries_remaining)
{
    if (!m_remote_id.has_value()) {
        settle_cache_batch(realm, promise, result_kind, batch_cache_operations(realm, *m_request_response_list, operations));
        return;
    }
    if (staging) {
        if (staging->budget()->is_discarded())
            return;
        staging->finish_preparation();
    }

    auto snapshot_callback = GC::create_function(GC::Heap::the(),
        GC::captures(*this, realm, operations, promise, result_kind, staging, retries_remaining),
        [](auto& self, auto& realm, auto& operations, auto& promise, auto& result_kind, auto& staging, auto& retries_remaining, GC::Ref<RequestResponseList> snapshot, IPC::Revision revision) {
            ArmedScopeGuard finish = [&] {
                if (staging)
                    staging->discard();
            };
            auto result = batch_cache_operations(realm, snapshot, operations);
            if (result.is_error() || (result_kind == BatchResult::Removed && !result.value())) {
                self.settle_cache_batch(realm, promise, result_kind, move(result));
                return;
            }

            auto owner = self.m_owner.ptr();
            if (!cache_storage_environment_is_live(owner)) {
                queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
                return;
            }
            auto bytes = encode_cache_snapshot(snapshot, owner->origin(), IPC::maximum_snapshot_size);
            if (bytes.is_error()) {
                auto error = bytes.release_error();
                if (error.is_errno() && error.code() == E2BIG)
                    reject_cache_storage_request(promise, IPC::StorageError::QuotaExceeded);
                else
                    WebIDL::reject_promise(promise, WebIDL::OperationError::create("Could not encode the Cache Storage snapshot"_utf16));
                return;
            }

            auto reply_callback = GC::create_function(GC::Heap::the(),
                GC::captures(self, realm, operations, promise, result_kind, staging, retries_remaining, result.release_value()),
                [](auto& self, auto& realm, auto& operations, auto& promise, auto& result_kind, auto& staging, auto& retries_remaining, auto& removed, IPC::Reply reply) {
                    ArmedScopeGuard finish = [&] {
                        if (staging)
                            staging->discard();
                    };
                    // The Hub applied the operation, so its promise settles with the result even if the promise's
                    // realm or the cache's owner has gone away since.
                    if (reply.has<IPC::Committed>()) {
                        settle_cache_batch(realm, promise, result_kind, removed);
                        return;
                    }
                    if (!cache_storage_promise_is_live(promise) || !cache_storage_environment_is_live(self.m_owner.ptr())) {
                        queue_cache_storage_rejection(promise, IPC::StorageError::Unavailable);
                        return;
                    }

                    auto* error = reply.get_pointer<IPC::StorageError>();
                    if (error && *error == IPC::StorageError::StaleRevision && retries_remaining > 0) {
                        // Retry the prepared operations without fetching or consuming a response again.
                        finish.disarm();
                        self.run_cache_batch(realm, operations, promise, result_kind, move(staging), retries_remaining - 1);
                        return;
                    }
                    queue_cache_storage_rejection(promise, error ? *error : IPC::StorageError::Unavailable);
                });
            if (auto error = send_cache_storage_request(self.m_owner.ptr(), IPC::Commit { *self.m_remote_id, revision, { bytes.release_value() } }, reply_callback); error.has_value()) {
                queue_cache_storage_rejection(promise, *error);
                return;
            }
            finish.disarm();
        });
    read_snapshot(promise, snapshot_callback, move(staging));
}

void Cache::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request_response_list);
}

// https://w3c.github.io/ServiceWorker/#cache-match
void Cache::match(JS::Realm& realm, Fetch::RequestInfo request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 2. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, promise, move(request), options), [](auto& self, auto& realm, auto& promise, auto& request, auto& options) {
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Let p be the result of running the algorithm specified in matchAll(request, options) method with request and options.
        // 2. Wait until p settles.
        auto match_all_promise = WebIDL::create_promise(realm);
        self.match_all(realm, move(request), options, match_all_promise);
        WebIDL::react_to_promise(match_all_promise,
            // 4. Else if p resolves with an array, responses, then:
            GC::create_function(GC::Heap::the(), GC::captures(realm, promise), [](auto& realm, auto& promise, JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. If responses is an empty array, then:
                if (auto& responses = value.as<JS::Array>(); responses.indexed_array_like_size() == 0) {
                    // 1. Resolve promise with undefined.
                    WebIDL::resolve_promise(promise);
                }
                // 2. Else:
                else {
                    // 1. Resolve promise with the first element of responses.
                    auto first_element = TRY(responses.get(0));
                    WebIDL::resolve_promise(promise, first_element);
                }

                return JS::js_undefined();
            }),

            // 3. If p rejects with an exception, then:
            GC::create_function(GC::Heap::the(), GC::captures(realm, promise), [](auto& realm, auto& promise, JS::Value exception) -> WebIDL::ExceptionOr<JS::Value> {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // 1. Reject promise with that exception.
                WebIDL::reject_promise(promise, exception);

                return JS::js_undefined();
            }));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-matchall
void Cache::match_all(JS::Realm& realm, Optional<Fetch::RequestInfo> request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;
    bool completed_synchronously = false;

    // 2. If the optional argument request is not omitted, then:
    if (request.has_value()) {
        auto result = request->visit(
            // 1. If request is a Request object, then:
            [&](GC::Ref<Fetch::Request> request) -> WebIDL::ExceptionOr<void> {
                // 1. Set r to request’s request.
                inner_request = request->request();

                // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with an
                //    empty array.
                if (inner_request->method() != "GET"sv && !options.ignore_method) {
                    WebIDL::resolve_promise(promise, MUST(JS::Array::create(realm, 0)));
                    completed_synchronously = true;
                    return {};
                }

                return {};
            },
            // 2. Else if request is a string, then:
            [&](Utf16String const& request) -> WebIDL::ExceptionOr<void> {
                // 1. Set r to the associated request of the result of invoking the initial value of Request as
                //    constructor with request as its argument. If this throws an exception, return a promise rejected
                //    with that exception.
                auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
                if (request_object.is_error()) {
                    WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                    return {};
                }

                inner_request = request_object.value()->request();
                return {};
            });

        if (result.is_error()) {
            WebIDL::reject_promise_with_exception(promise, result.release_error());
            return;
        }

        if (completed_synchronously || !inner_request)
            return;
    }

    // 5. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, inner_request, promise, options), [](auto& self, auto& realm, auto& inner_request, auto& promise, auto& options) {
        self.read_snapshot(promise, GC::create_function(GC::Heap::the(), GC::captures(realm, inner_request, promise, options, self.m_remote_id.has_value()), [](auto& realm, auto& inner_request, auto& promise, auto& options, auto& shared_cache, GC::Ref<RequestResponseList> snapshot, IPC::Revision) {
            // 1. Let responses be an empty list.
            auto responses = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Fetch::Infrastructure::ResponseView>>>();

            // 2. If the optional argument request is omitted, then:
            if (!inner_request) {
                // 1. For each requestResponse of the relevant request response list:
                for (auto& request_response : snapshot->elements()) {
                    // 1. Add a copy of requestResponse’s response to responses.
                    responses->elements().append(request_response->response->clone(realm));
                }
            }
            // 3. Else:
            else {
                // 1. Let requestResponses be the result of running Query Cache with r and options.
                auto request_responses = query_cache(realm, snapshot, *inner_request, options);

                // 2. For each requestResponse of requestResponses:
                for (auto request_response : request_responses->elements()) {
                    // 1. Add a copy of requestResponse’s response to responses.
                    // NB: No need to copy. Query Cache creates a copy, and the requestResponses list is dropped hereafter.
                    responses->elements().append(request_response->response);
                }
            }

            // 3. For each response of responses:
            auto& settings = HTML::relevant_settings_object(promise->promise());
            for (auto response : responses->elements()) {
                // 1. If response’s type is "opaque" and cross-origin resource policy check with promise’s relevant settings
                //    object’s origin, promise’s relevant settings object, "", and response’s internal response returns
                //    blocked, then reject promise with a TypeError and abort these steps.
                // FIXME: Apply this check to the legacy renderer-local cache as well.
                if (shared_cache && cached_response_is_blocked_by_cross_origin_policy(settings.origin(), settings.policy_container()->embedder_policy, *response->record())) {
                    WebIDL::reject_promise(promise, JS::TypeError::create(realm, "The cached response was blocked by its cross-origin resource policy"sv));
                    return;
                }
            }

            // 4. Queue a task, on promise’s relevant settings object’s responsible event loop using the DOM manipulation
            //    task source, to perform the following steps:
            HTML::queue_a_task(
                HTML::Task::Source::DOMManipulation,
                HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
                {},
                GC::create_function(GC::Heap::the(), GC::captures(realm, promise, responses), [](auto& realm, auto& promise, auto& responses) {
                    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    resolve_cache_response_list(realm, promise, responses);
                }));
        }));
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-add
void Cache::add(JS::Realm& realm, Fetch::RequestInfo request, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let requests be an array containing only request.
    // 2. Let responseArrayPromise be the result of running the algorithm specified in addAll(requests) passing requests
    //    as the argument.
    auto add_all_promise = WebIDL::create_promise(realm);
    add_all(realm, { { request } }, add_all_promise);

    // 3. Return the result of reacting to responseArrayPromise with a fulfillment handler that returns undefined.
    WebIDL::react_to_promise(add_all_promise,
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            WebIDL::resolve_promise(promise);
            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(promise), [](auto& promise, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-addAll
void Cache::add_all(JS::Realm& realm, ReadonlySpan<Fetch::RequestInfo> requests, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let responsePromises be an empty list.
    auto response_promises = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<WebIDL::Promise>>>();

    // 2. Let requestList be an empty list.
    Vector<NonnullRefPtr<Fetch::Infrastructure::Request>> request_list;

    // 3. For each request whose type is Request in requests:
    for (auto const& request_info : requests) {
        if (auto const* request = request_info.get_pointer<GC::Ref<Fetch::Request>>()) {
            // 1. Let r be request’s request.
            auto inner_request = (*request)->request();

            // 2. If r’s url’s scheme is not one of "http" and "https", or r’s method is not `GET`, return a promise
            //    rejected with a TypeError.
            if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv) {
                WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));
                return;
            }
        }
    }

    // A valid batch adds one distinct entry per request. Reject batches that cannot fit before constructing or fetching requests.
    if (m_remote_id.has_value() && requests.size() > maximum_cache_snapshot_entries) {
        reject_cache_storage_request(promise, IPC::StorageError::QuotaExceeded);
        return;
    }

    // 4. Let fetchControllers be a list of fetch controllers.
    auto fetch_controllers = GC::Heap::the().allocate<CacheFetchControllers>();
    RefPtr<CacheBodyStaging> staging;
    GC::Ptr<GC::HeapVector<GC::Ref<GC::Function<void()>>>> pending_fetches;
    GC::Ptr<WebIDL::ReactionSteps> ignore_failed_fetch;
    if (m_remote_id.has_value()) {
        staging = CacheBodyStaging::create();
        if (!staging->observe_preparation(m_owner.ptr(), promise, fetch_controllers)) {
            reject_cache_storage_request(promise, IPC::StorageError::Unavailable);
            return;
        }
        pending_fetches = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<GC::Function<void()>>>>();
        ignore_failed_fetch = GC::create_function(GC::Heap::the(), [](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
            return JS::js_undefined();
        });
    }

    // 5. For each request in requests:
    for (auto const& request_info : requests) {
        // 1. Let r be the associated request of the result of invoking the initial value of Request as constructor with
        //    request as its argument. If this throws an exception, return a promise rejected with that exception.
        auto result = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request_info);
        if (result.is_error()) {
            if (staging)
                staging->discard();
            WebIDL::reject_promise_with_exception(promise, result.release_error());
            return;
        }

        auto inner_request = result.value()->request();

        // 2. If r’s url’s scheme is not one of "http" and "https", then:
        if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv)) {
            if (staging)
                staging->discard();
            // 1. For each fetchController of fetchControllers, abort fetchController.
            for (auto fetch_controller : fetch_controllers->elements())
                fetch_controller->abort(realm, {});

            // 2. Return a promise rejected with a TypeError.
            WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must have an HTTP(S) URL"sv));
            return;
        }

        // 3. If r’s client’s global object is a ServiceWorkerGlobalScope object, set request’s service-workers mode to "none".
        auto client = Fetch::Fetching::resolve_client(inner_request);
        if (Bindings::service_worker_global_scope_from_global_object(client->global_object()))
            inner_request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::None);

        // 4. Add r to requestList.
        request_list.append(inner_request);

        // 5. Let responsePromise be a new promise.
        auto response_promise = WebIDL::create_promise(realm);

        // 6. Run the following substeps in parallel:
        auto fetch = GC::create_function(GC::Heap::the(), GC::captures(realm, fetch_controllers, inner_request, response_promise, staging, ignore_failed_fetch), [](auto& realm, auto& fetch_controllers, auto& inner_request, auto& response_promise, auto& staging, auto& ignore_failed_fetch) {
            if (staging && staging->budget()->is_discarded())
                return;

            // * Append the result of fetching r.
            Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {
                staging ? Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent : Fetch::Engine::BodyIntent::ConsumeForCallback
            };

            // * To processResponse for response, run these substeps:
            fetch_algorithms_input.process_response = GC::create_function(GC::Heap::the(), GC::captures(realm, fetch_controllers, response_promise, staging), [](auto& realm, auto& fetch_controllers, auto& response_promise, auto& staging, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                if (staging && staging->budget()->is_discarded())
                    return;

                // 1. If response’s type is "error", or response’s status is not an ok status or is 206, reject
                //    responsePromise with a TypeError.
                auto status = response->status();
                if (response->type() == Fetch::Infrastructure::Response::Type::Error || !Fetch::Infrastructure::is_ok_status(status) || status == 206) {
                    WebIDL::reject_promise(response_promise, JS::TypeError::create(realm, "Fetch request failed"_utf16));
                    return;
                }

                // 2. If a field value of the response's Vary header matches "*", reject and abort the batch.
                bool found_vary_wildcard = false;
                response->header_list()->for_each_vary_header([&](StringView field_value) {
                    found_vary_wildcard = field_value == "*"sv;
                    return found_vary_wildcard ? IterationDecision::Break : IterationDecision::Continue;
                });
                if (found_vary_wildcard) {
                    WebIDL::reject_promise(response_promise, JS::TypeError::create(realm, "Vary '*' is not supported"_utf16));
                    if (!staging) {
                        for (auto fetch_controller : fetch_controllers->elements())
                            fetch_controller->abort(realm, {});
                    }
                    return;
                }

                if (staging && response->aborted()) {
                    WebIDL::reject_promise(response_promise, WebIDL::AbortError::create("Fetch request was aborted"_utf16));
                    return;
                }
                if (staging)
                    materialize_cache_response(realm, response, response_promise, move(staging), CacheResponseSource::Fetch);
            });

            if (!staging)
                fetch_algorithms_input.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(realm, response_promise), [](auto& realm, auto& response_promise, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bytes) {
                    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    if (response->aborted()) {
                        WebIDL::reject_promise(response_promise, WebIDL::AbortError::create("Fetch request was aborted"_utf16));
                        return;
                    }

                    if (bytes.has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>()) {
                        WebIDL::reject_promise(response_promise, JS::TypeError::create(realm, "Fetch response body failed"sv));
                        return;
                    }

                    auto record = response->record()->clone();
                    if (auto* body = bytes.get_pointer<Core::ImmutableBytes>())
                        record->unsafe_response()->set_body(Fetch::Infrastructure::ResponseBody::from_bytes(move(*body)));
                    WebIDL::resolve_promise(response_promise, Fetch::Infrastructure::ResponseView::create(realm, move(record)));
                });

            auto fetch_controller = Fetch::Fetching::fetch(realm, inner_request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
            fetch_controllers->elements().append(fetch_controller);

            // Note: The cache commit is allowed when the response’s body is fully received.
            if (staging) {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::react_to_promise(response_promise,
                    GC::create_function(GC::Heap::the(), GC::captures(fetch_controllers, fetch_controller), [](auto& fetch_controllers, auto& fetch_controller, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                        fetch_controllers->elements().remove_first_matching([&](auto controller) { return controller == fetch_controller; });
                        return JS::js_undefined();
                    }),
                    ignore_failed_fetch);
            }
        });
        if (pending_fetches)
            pending_fetches->elements().append(fetch);
        else
            Platform::EventLoopPlugin::the().deferred_invoke(fetch);

        // 7. Add responsePromise to responsePromises.
        response_promises->elements().append(response_promise);
    }

    if (pending_fetches && !pending_fetches->elements().is_empty()) {
        // A new fetch can start as soon as any response body has been fully read.
        auto slots = staging->reserve_fetch_slots(request_list.size());
        if (!slots) {
            staging->discard();
            reject_cache_storage_request(promise, IPC::StorageError::QuotaExceeded);
            return;
        }
        if (slots < pending_fetches->elements().size()) {
            auto start_next_fetch = GC::create_function(GC::Heap::the(), GC::captures(pending_fetches, slots), [](auto& pending_fetches, auto& next_fetch, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                if (next_fetch < pending_fetches->elements().size())
                    Platform::EventLoopPlugin::the().deferred_invoke(pending_fetches->elements()[next_fetch++]);
                return JS::js_undefined();
            });
            for (auto response_promise : response_promises->elements())
                WebIDL::react_to_promise(response_promise, start_next_fetch, ignore_failed_fetch);
        }
        for (size_t i = 0; i < slots; ++i)
            Platform::EventLoopPlugin::the().deferred_invoke(pending_fetches->elements()[i]);
    }

    // 6. Let p be the result of getting a promise to wait for all of responsePromises.
    auto wait_for_responses_promise = WebIDL::get_promise_for_wait_for_all(realm, response_promises->elements());

    // 7. Return the result of reacting to p with a fulfillment handler that, when called with argument responses, performs the following substeps:
    WebIDL::react_to_promise(wait_for_responses_promise,
        GC::create_function(GC::Heap::the(), GC::captures(*this, realm, move(request_list), promise, staging), [](auto& self, auto& realm, auto& request_list, auto& promise, auto& staging, JS::Value result) -> WebIDL::ExceptionOr<JS::Value> {
            HTML::TemporaryExecutionContext context { realm };
            if (staging) {
                if (staging->budget()->is_discarded())
                    return JS::js_undefined();
                staging->release_fetch_slots();
            }
            auto& responses = result.as<JS::Array>();

            // 1. Let operations be an empty list.
            auto operations = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();

            // 2. Let index be zero.
            // 3. For each response in responses:
            for (size_t index = 0; index < responses.indexed_array_like_size(); ++index) {
                auto response_value = TRY(responses.get(index));
                auto& response = as<Fetch::Infrastructure::ResponseView>(response_value.as_cell());

                // 1. Let operation be a cache batch operation.
                auto operation = GC::Heap::the().allocate<CacheBatchOperation>(
                    // 2. Set operation’s type to "put".
                    CacheBatchOperation::Type::Put,
                    // 3. Set operation’s request to requestList[index].
                    request_list[index],
                    // 4. Set operation’s response to response.
                    response);

                // 5. Append operation to operations.
                operations->elements().append(operation);

                // 6. Increment index by one.
            }

            // 5. Let cacheJobPromise be a new promise.
            // 6. Run the following substeps in parallel:
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(self, realm, operations, promise, move(staging)), [](auto& self, auto& realm, auto& operations, auto& promise, auto& staging) {
                self.run_cache_batch(realm, operations, promise, BatchResult::Undefined, move(staging));
            }));

            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(realm, fetch_controllers, promise, staging), [](auto& realm, auto& fetch_controllers, auto& promise, auto& staging, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            if (staging && staging->budget()->is_discarded())
                return JS::js_undefined();
            for (auto fetch_controller : fetch_controllers->elements())
                fetch_controller->abort(realm, {});
            if (staging)
                staging->discard();
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-put
void Cache::put(JS::Realm& realm, Fetch::RequestInfo request, GC::Ref<Fetch::Response> response, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let innerRequest be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;

    auto request_result = request.visit(
        // 2. If request is a Request object, then set innerRequest to request’s request.
        [&](GC::Ref<Fetch::Request> request) -> WebIDL::ExceptionOr<void> {
            inner_request = request->request();
            return {};
        },
        // 3. Else:
        [&](Utf16String const& request) -> WebIDL::ExceptionOr<void> {
            // 1. Let requestObj be the result of invoking Request’s constructor with request as its argument. If this
            //    throws an exception, return a promise rejected with exception.
            auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
            if (request_object.is_error()) {
                WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                return {};
            }

            inner_request = request_object.value()->request();
            return {};
        });

    if (request_result.is_error()) {
        WebIDL::reject_promise_with_exception(promise, request_result.release_error());
        return;
    }

    if (!inner_request)
        return;

    // 4. If innerRequest’s url’s scheme is not one of "http" and "https", or innerRequest’s method is not `GET`, return
    //    a promise rejected with a TypeError.
    if (!inner_request->url().scheme().is_one_of("http"sv, "https"sv) || inner_request->method() != "GET"sv) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Request must be a GET request with an HTTP(S) URL"sv));
        return;
    }

    // 5. Let innerResponse be response’s response.
    auto inner_response = response->response();

    // 6. If innerResponse’s status is 206, return a promise rejected with a TypeError.
    if (inner_response->status() == 206) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Partial responses are not supported"sv));
        return;
    }

    // 7. If innerResponse’s header list contains a header named `Vary`, then:
    //     1. Let fieldValues be the list containing the items corresponding to the Vary header’s field-values.
    //     2. For each fieldValue in fieldValues:
    bool found_vary_wildcard = false;

    inner_response->header_list()->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", return a promise rejected with a TypeError.
        found_vary_wildcard = field_value == "*"sv;
        return found_vary_wildcard ? IterationDecision::Break : IterationDecision::Continue;
    });

    if (found_vary_wildcard) {
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Vary '*' is not supported"sv));
        return;
    }

    // 8. If innerResponse’s body is disturbed or locked, return a promise rejected with a TypeError.
    if (auto body = inner_response->body()) {
        if (auto stream = body->stream(); stream->is_disturbed() || stream->is_locked()) {
            WebIDL::reject_promise(promise, JS::TypeError::create(realm, "Response's body stream is disturbed or locked"sv));
            return;
        }
    }

    // 9-11. Copy the response and read its body before committing the operation.
    auto body_read_promise = WebIDL::create_promise(realm);
    RefPtr<CacheBodyStaging> staging;
    if (m_remote_id.has_value()) {
        staging = CacheBodyStaging::create();
        if (!staging->observe_preparation(m_owner.ptr(), promise)) {
            reject_cache_storage_request(promise, IPC::StorageError::Unavailable);
            return;
        }
    }
    materialize_cache_response(realm, inner_response, body_read_promise, staging, CacheResponseSource::Put);

    // 19. Return the result of the fulfillment of bodyReadPromise:
    WebIDL::react_to_promise(body_read_promise,
        GC::create_function(GC::Heap::the(), GC::captures(*this, realm, inner_request, promise, staging), [](auto& self, auto& realm, auto& inner_request, auto& promise, auto& staging, JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            HTML::TemporaryExecutionContext context { realm };

            auto operations = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();
            auto& cloned_response = as<Fetch::Infrastructure::ResponseView>(value.as_cell());
            operations->elements().append(GC::Heap::the().allocate<CacheBatchOperation>(CacheBatchOperation::Type::Put, inner_request.release_nonnull(), cloned_response));

            // 2. Return cacheJobPromise and run these steps in parallel:
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(self, realm, operations, promise, move(staging)), [](auto& self, auto& realm, auto& operations, auto& promise, auto& staging) {
                self.run_cache_batch(realm, operations, promise, BatchResult::Undefined, move(staging));
            }));

            return JS::js_undefined();
        }),
        GC::create_function(GC::Heap::the(), GC::captures(promise, staging), [](auto& promise, auto& staging, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            if (staging && staging->budget()->is_discarded())
                return JS::js_undefined();
            if (staging)
                staging->discard();
            WebIDL::reject_promise(promise, reason);
            return JS::js_undefined();
        }));
}

// https://w3c.github.io/ServiceWorker/#cache-delete
void Cache::delete_(JS::Realm& realm, Fetch::RequestInfo request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;
    bool completed_synchronously = false;

    auto result = request.visit(
        // 2. If request is a Request object, then:
        [&](GC::Ref<Fetch::Request> request) -> WebIDL::ExceptionOr<void> {
            // 1. Set r to request’s request.
            inner_request = request->request();

            // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with false.
            if (inner_request->method() != "GET"sv && !options.ignore_method) {
                WebIDL::resolve_promise(promise, JS::Value { false });
                completed_synchronously = true;
                return {};
            }

            return {};
        },
        // 3. Else if request is a string, then:
        [&](Utf16String const& request) -> WebIDL::ExceptionOr<void> {
            // 1. Set r to the associated request of the result of invoking the initial value of Request as constructor
            //    with request as its argument. If this throws an exception, return a promise rejected with that
            //    exception.
            auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
            if (request_object.is_error()) {
                WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                return {};
            }

            inner_request = request_object.value()->request();
            return {};
        });

    if (result.is_error()) {
        WebIDL::reject_promise_with_exception(promise, result.release_error());
        return;
    }

    if (completed_synchronously || !inner_request)
        return;

    // 4. Let operations be an empty list.
    auto operations = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<CacheBatchOperation>>>();

    // 5. Let operation be a cache batch operation.
    auto operation = GC::Heap::the().allocate<CacheBatchOperation>(
        // 6. Set operation’s type to "delete".
        CacheBatchOperation::Type::Delete,
        // 7. Set operation’s request to r.
        *inner_request,
        nullptr,
        // 8. Set operation’s options to options.
        options);

    // 9. Append operation to operations.
    operations->elements().append(operation);
    // 12. Run the following substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, operations, promise), [](auto& self, auto& realm, auto& operations, auto& promise) {
        self.run_cache_batch(realm, operations, promise, BatchResult::Removed);
    }));
}

// https://w3c.github.io/ServiceWorker/#cache-keys
void Cache::keys(JS::Realm& realm, Optional<Fetch::RequestInfo> request, CacheQueryOptions options, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let r be null.
    RefPtr<Fetch::Infrastructure::Request> inner_request;
    bool completed_synchronously = false;

    // 2. If the optional argument request is not omitted, then:
    if (request.has_value()) {
        auto result = request->visit(
            // 1. If request is a Request object, then:
            [&](GC::Ref<Fetch::Request> request) -> WebIDL::ExceptionOr<void> {
                // 1. Set r to request’s request.
                inner_request = request->request();

                // 2. If r’s method is not `GET` and options.ignoreMethod is false, return a promise resolved with an
                //    empty array.
                if (inner_request->method() != "GET"sv && !options.ignore_method) {
                    WebIDL::resolve_promise(promise, MUST(JS::Array::create(realm, 0)));
                    completed_synchronously = true;
                    return {};
                }

                return {};
            },
            // 2. Else if request is a string, then:
            [&](Utf16String const& request) -> WebIDL::ExceptionOr<void> {
                // 1. Set r to the associated request of the result of invoking the initial value of Request as
                //    constructor with request as its argument. If this throws an exception, return a promise rejected
                //    with that exception.
                auto request_object = Fetch::Request::create_with_settings(HTML::principal_realm_settings_object(realm), request);
                if (request_object.is_error()) {
                    WebIDL::reject_promise_with_exception(promise, request_object.release_error());
                    return {};
                }

                inner_request = request_object.value()->request();
                return {};
            });

        if (result.is_error()) {
            WebIDL::reject_promise_with_exception(promise, result.release_error());
            return;
        }

        if (completed_synchronously || !inner_request)
            return;
    }

    // 5. Run these substeps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this, realm, inner_request, promise, options), [](auto& self, auto& realm, auto& inner_request, auto& promise, auto& options) {
        self.read_snapshot(promise, GC::create_function(GC::Heap::the(), GC::captures(realm, inner_request, promise, options), [](auto& realm, auto& inner_request, auto& promise, auto& options, GC::Ref<RequestResponseList> snapshot, IPC::Revision) {
            // 1. Let requests be an empty list.
            Vector<NonnullRefPtr<Fetch::Infrastructure::Request>> requests;

            // 2. If the optional argument request is omitted, then:
            if (!inner_request) {
                // 1. For each requestResponse of the relevant request response list:
                for (auto& request_response : snapshot->elements()) {
                    // 1. Add requestResponse’s request to requests.
                    requests.append(request_response->request);
                }
            }
            // 3. Else:
            else {
                // 1. Let requestResponses be the result of running Query Cache with r and options.
                // NB: Only the requests are used, so we copy them below. Copying the responses would tee their bodies.
                auto request_responses = query_cache(realm, snapshot, *inner_request, options, CloneCache::No);

                // 2. For each requestResponse of requestResponses:
                for (auto request_response : request_responses->elements()) {
                    // 1. Add requestResponse’s request to requests.
                    requests.append(request_response->request->clone());
                }
            }

            // 4. Queue a task, on promise’s relevant settings object’s responsible event loop using the DOM manipulation
            //    task source, to perform the following steps:
            HTML::queue_a_task(
                HTML::Task::Source::DOMManipulation,
                HTML::relevant_settings_object(promise->promise()).responsible_event_loop(),
                {},
                GC::create_function(GC::Heap::the(), GC::captures(realm, promise, move(requests)), [](auto& realm, auto& promise, auto& requests) {
                    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    resolve_cache_request_list(realm, promise, requests);
                }));
        }));
    }));
}

// https://w3c.github.io/ServiceWorker/#request-matches-cached-item-algorithm
static bool request_matches_cached_item(
    NonnullRefPtr<Fetch::Infrastructure::Request> request_query,
    NonnullRefPtr<Fetch::Infrastructure::Request> request,
    GC::Ptr<Fetch::Infrastructure::ResponseView> response,
    CacheQueryOptions options)
{
    // 1. If options["ignoreMethod"] is false and request’s method is not `GET`, return false.
    if (!options.ignore_method && request->method() != "GET"sv)
        return false;

    // 2. Let queryURL be requestQuery’s url.
    auto query_url = request_query->url();

    // 3. Let cachedURL be request’s url.
    auto cached_url = request->url();

    // 4. If options["ignoreSearch"] is true, then:
    if (options.ignore_search) {
        // 1. Set cachedURL’s query to the empty string.
        cached_url.set_query(""sv);

        // 2. Set queryURL’s query to the empty string.
        query_url.set_query(""sv);
    }

    // 5. If queryURL does not equal cachedURL with the exclude fragment flag set, then return false.
    if (!query_url.equals(cached_url, URL::ExcludeFragment::Yes))
        return false;

    // 6. If response is null, options["ignoreVary"] is true, or response’s header list does not contain `Vary`, then
    //    return true.
    if (!response || options.ignore_vary || !response->header_list()->contains("Vary"sv))
        return true;

    // 7. Let fieldValues be the list containing the elements corresponding to the field-values of the Vary header for
    //    the value of the header with name `Vary`.
    // 8. For each fieldValue in fieldValues:
    bool matches = true;

    response->header_list()->for_each_vary_header([&](StringView field_value) {
        // 1. If fieldValue matches "*", or the combined value given fieldValue and request’s header list does not match
        //    the combined value given fieldValue and requestQuery’s header list, then return false.
        matches = field_value == "*"sv
            || request->header_list()->get(field_value) == request_query->header_list()->get(field_value);
        return matches ? IterationDecision::Break : IterationDecision::Continue;
    });

    // 9. Return true.
    return matches;
}

// https://w3c.github.io/ServiceWorker/#query-cache-algorithm
GC::Ref<RequestResponseList> Cache::query_cache(JS::Realm& realm, GC::Ref<RequestResponseList> storage, NonnullRefPtr<Fetch::Infrastructure::Request> request_query, CacheQueryOptions options, CloneCache clone_cache)
{
    // 1. Let resultList be an empty list.
    auto result_list = GC::Heap::the().allocate<RequestResponseList>();

    // 2–4. The caller supplies targetStorage, defaulting to the relevant request response list.
    // 5. For each requestResponse of storage:
    for (auto request_response : storage->elements()) {
        // 1. Let cachedRequest be requestResponse’s request.
        auto cached_request = request_response->request;

        // 2. Let cachedResponse be requestResponse’s response.
        auto cached_response = request_response->response;

        // 3. If Request Matches Cached Item with requestQuery, cachedRequest, cachedResponse, and options returns true, then:
        if (request_matches_cached_item(request_query, cached_request, cached_response, options)) {
            // AD-HOC: Not all callers actually need a copy. In Batch Cache Operations, it becomes much easier to remove
            //         matching items from the cache if we can do a pointer comparison.
            if (clone_cache == CloneCache::No) {
                result_list->elements().append(request_response);
                continue;
            }

            // 1. Let requestCopy be a copy of cachedRequest.
            auto request_copy = cached_request->clone();

            // 2. Let responseCopy be a copy of cachedResponse.
            auto response_copy = cached_response->clone(realm);

            // 3. Add requestCopy/responseCopy to resultList.
            result_list->elements().append(GC::Heap::the().allocate<RequestResponse>(request_copy, response_copy));
        }
    }

    // 6. Return resultList.
    return result_list;
}

// https://w3c.github.io/ServiceWorker/#batch-cache-operations-algorithm
WebIDL::ExceptionOr<bool> Cache::batch_cache_operations(JS::Realm& realm, GC::Ref<RequestResponseList> cache, GC::Ref<BatchOperations> operations)
{
    // 1. The caller supplies the relevant request response list.

    // 2. Let backupCache be a new request response list that is a copy of cache.
    // NB: This copies the list, not its items. The operations below only add and remove items, so restoring the list
    //     undoes them, and cloning the responses would tee the body of every cached response on every operation.
    auto backup_cache = GC::Heap::the().allocate<RequestResponseList>();
    backup_cache->elements().extend(cache->elements());

    // 3. Let addedItems be an empty list.
    auto added_items = GC::Heap::the().allocate<RequestResponseList>();

    // 4. Try running the following substeps atomically:
    auto result = [&]() -> WebIDL::ExceptionOr<bool> {
        // 1. Let resultList be an empty list.
        // NB: This result list is unused, only Cache.delete needs to know if there were any results.
        bool removed_any_items = false;

        auto remove_items_from_cache = [&](auto request_responses) {
            removed_any_items |= cache->elements().remove_all_matching([&](GC::Ref<RequestResponse> request_response) {
                return request_responses->elements().contains_slow(request_response);
            });
        };

        // 2. For each operation in operations:
        for (auto operation : operations->elements()) {
            // 1. If operation’s type matches neither "delete" nor "put", throw a TypeError.
            if (operation->type != CacheBatchOperation::Type::Delete && operation->type != CacheBatchOperation::Type::Put)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operation type must be 'delete' or 'put'"_utf16 };

            // 2. If operation’s type matches "delete" and operation’s response is not null, throw a TypeError.
            if (operation->type == CacheBatchOperation::Type::Delete && operation->response)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operations of type 'delete' must not have a response"_utf16 };

            // 3. If the result of running Query Cache with operation’s request, operation’s options, and addedItems is
            //    not empty, throw an "InvalidStateError" DOMException.
            if (!query_cache(realm, added_items, operation->request, operation->options, CloneCache::No)->elements().is_empty())
                return WebIDL::InvalidStateError::create("Batch operation requests must be unique"_utf16);

            // 4. Let requestResponses be an empty list.
            GC::Ptr<RequestResponseList> request_responses;

            // 5. If operation’s type matches "delete", then:
            if (operation->type == CacheBatchOperation::Type::Delete) {
                // 1. Set requestResponses to the result of running Query Cache with operation’s request and operation’s options.
                request_responses = query_cache(realm, cache, operation->request, operation->options, CloneCache::No);

                // 2. For each requestResponse in requestResponses:
                //     1. Remove the item whose value matches requestResponse from cache.
                remove_items_from_cache(request_responses);
            }
            // 6. Else if operation’s type matches "put", then:
            else if (operation->type == CacheBatchOperation::Type::Put) {
                // 1. If operation’s response is null, throw a TypeError.
                if (!operation->response)
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Batch operations of type 'put' must have a response"_utf16 };

                // 2. Let r be operation’s request’s associated request.
                auto request = operation->request;

                // 3. If r’s url’s scheme is not one of "http" and "https", throw a TypeError.
                if (!request->url().scheme().is_one_of("http"sv, "https"sv))
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request must be a GET request with an HTTP(S) URL"_utf16 };

                // 4. If r’s method is not `GET`, throw a TypeError.
                if (request->method() != "GET"sv)
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Request must be a GET request with an HTTP(S) URL"_utf16 };

                // 5. If operation’s options is not null, throw a TypeError.
                // FIXME: Spec issue: No other part of the spec indicates that options may be created as null.

                // 6. Set requestResponses to the result of running Query Cache with operation’s request.
                request_responses = query_cache(realm, cache, operation->request, {}, CloneCache::No);

                // 7. For each requestResponse of requestResponses:
                //     1. Remove the item whose value matches requestResponse from cache.
                remove_items_from_cache(request_responses);

                // 8. Append operation’s request/operation’s response to cache.
                cache->elements().append(GC::Heap::the().allocate<RequestResponse>(operation->request, *operation->response));

                // FIXME: 9. If the cache write operation in the previous two steps failed due to exceeding the granted quota
                //           limit, throw a QuotaExceededError.

                // 10. Append operation’s request/operation’s response to addedItems.
                added_items->elements().append(GC::Heap::the().allocate<RequestResponse>(operation->request, *operation->response));
            }

            // 7. Append operation’s request/operation’s response to resultList.
        }

        // 3. Return resultList.
        return removed_any_items;
    }();

    // 5. And then, if an exception was thrown, then:
    if (result.is_error()) {
        // 1. Remove all the items from the relevant request response list.
        // 2. For each requestResponse of backupCache:
        //     1. Append requestResponse to the relevant request response list.
        // NB: The list is shared with every other Cache object for this cache, so restore it in place.
        cache->elements().clear();
        cache->elements().extend(backup_cache->elements());

        // 3. Throw the exception.
        return result.release_error();

        // Note: When an exception is thrown, the implementation does undo (roll back) any changes made to the cache
        //       storage during the batch operation job.
    }

    return result.release_value();
}

void CacheBatchOperation::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(response);
}

}
