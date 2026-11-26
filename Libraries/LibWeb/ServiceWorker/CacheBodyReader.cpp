/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/CacheBodyReader.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::ServiceWorker {

// This counts staged body bytes, separate from the stream and transport queues and buffer capacity overhead.
static constexpr size_t maximum_staged_body_bytes = 16 * MiB;
static Atomic<size_t> s_staged_body_bytes { 0 };

NonnullRefPtr<CacheBodyBudget> CacheBodyBudget::create(size_t maximum_bytes)
{
    return adopt_ref(*new CacheBodyBudget(min(maximum_bytes, maximum_cache_body_bytes)));
}

CacheBodyBudget::~CacheBodyBudget()
{
    VERIFY(m_pending_reads.is_empty());
    release(m_reserved_bytes);
}

bool CacheBodyBudget::try_reserve(size_t bytes)
{
    if (m_discarded || bytes > m_maximum_bytes - m_reserved_bytes)
        return false;

    auto staged = s_staged_body_bytes.load(AK::MemoryOrder::memory_order_relaxed);
    do {
        if (bytes > maximum_staged_body_bytes - staged)
            return false;
    } while (!s_staged_body_bytes.compare_exchange_strong(staged, staged + bytes, AK::MemoryOrder::memory_order_relaxed));

    m_reserved_bytes += bytes;
    return true;
}

void CacheBodyBudget::release(size_t bytes)
{
    VERIFY(bytes <= m_reserved_bytes);
    m_reserved_bytes -= bytes;
    auto previous = s_staged_body_bytes.fetch_sub(bytes, AK::MemoryOrder::memory_order_relaxed);
    VERIFY(previous >= bytes);
}

class CacheBodyReadRequest final : public Streams::ReadRequest {
    GC_CELL(CacheBodyReadRequest, Streams::ReadRequest);
    GC_DECLARE_ALLOCATOR(CacheBodyReadRequest);

public:
    CacheBodyReadRequest(JS::Realm& realm, Streams::ReadableStreamDefaultReader& reader, NonnullRefPtr<CacheBodyBudget> budget, GC::Ref<CacheBodyReadSuccess> success, GC::Ref<CacheBodyReadFailure> failure)
        : m_realm(realm)
        , m_reader(reader)
        , m_budget(move(budget))
        , m_success(success)
        , m_failure(failure)
    {
    }

    GC_ALLOW_CELL_DESTRUCTOR virtual ~CacheBodyReadRequest() override
    {
        discard_partial_body();
    }

    void start()
    {
        if (m_budget->is_discarded()) {
            discard();
            return;
        }
        m_pending_read.request = *this;
        m_budget->m_pending_reads.append(m_pending_read);
        read_next_chunk();
    }

    void discard()
    {
        discard_partial_body();
        auto failure = exchange(m_failure, {});
        m_success = nullptr;

        // Cancellation can synchronously settle the outstanding stream read. Its request is detached now.
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), GC::captures(m_realm, m_reader, failure), [](auto& realm, auto& reader, auto& failure) {
            HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            auto exception = WebIDL::AbortError::create("Cache Storage body read was discarded"_utf16);
            auto reason = Web::throw_completion(realm, exception).release_value();
            WebIDL::mark_promise_as_handled(reader->cancel(reason));
            failure->function()(reason, CacheBodyFailure::Stream);
        }));
    }

private:
    void read_next_chunk()
    {
        if (!m_budget)
            return;
        HTML::TemporaryExecutionContext context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        m_reader->read_a_chunk(*this);
    }

    virtual void on_chunk(JS::Value chunk) override
    {
        if (!m_budget)
            return;

        auto array = chunk.as_if<JS::Uint8Array>();
        if (!array) {
            fail(JS::TypeError::create(m_realm, "Chunk data is not Uint8Array"sv), CacheBodyFailure::Stream, true);
            return;
        }

        auto record = JS::make_typed_array_with_buffer_witness_record(*array, JS::ArrayBuffer::Order::SeqCst);
        auto length = JS::is_typed_array_out_of_bounds(record) ? 0 : JS::typed_array_byte_length(record);
        if (!m_budget->try_reserve(length)) {
            auto exception = WebIDL::QuotaExceededError::create("Cache Storage exceeded its body buffering limit"_utf16);
            fail(Web::throw_completion(m_realm, exception).release_value(), CacheBodyFailure::Quota, true);
            return;
        }

        auto space = m_bytes.get_bytes_for_writing(length);
        if (space.is_error()) {
            m_budget->release(length);
            fail(JS::InternalError::create(m_realm, "Could not allocate a Cache Storage body buffer"sv), CacheBodyFailure::Stream, true);
            return;
        }

        if (length) {
            auto& buffer = *array->viewed_array_buffer();
            auto offset = array->byte_offset();
            auto destination = space.release_value();
            if (!buffer.is_shared_array_buffer()) {
                buffer.copy_to(offset, destination);
            } else {
                for (size_t i = 0; i < length; ++i)
                    destination[i] = static_cast<u8>(buffer.get_value<u8>(offset + i, true, JS::ArrayBuffer::Order::Unordered).as_double());
            }
        }

        // A queued stream can fulfill each read synchronously, so do not recurse through its controller.
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), GC::captures(*this), [](auto& request) {
            request.read_next_chunk();
        }));
    }

    virtual void on_close() override
    {
        if (!m_budget)
            return;

        // Keep the charge through the callback, which transfers the bytes to the caller.
        m_budget->m_pending_reads.remove(m_pending_read);
        auto budget = exchange(m_budget, nullptr);
        auto bytes = move(m_bytes);
        auto success = exchange(m_success, {});
        m_failure = nullptr;
        HTML::TemporaryExecutionContext context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        success->function()(move(bytes));
    }

    virtual void on_error(JS::Value error) override
    {
        fail(error, CacheBodyFailure::Stream, false);
    }

    void fail(JS::Value reason, CacheBodyFailure failure, bool cancel_reader)
    {
        if (!m_budget)
            return;
        discard_partial_body();
        auto failure_callback = exchange(m_failure, {});
        m_success = nullptr;

        if (cancel_reader) {
            // Cancel only this reader's stream and do not wait for another branch of a tee to finish.
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function<void()>(heap(), GC::captures(m_realm, m_reader, reason), [](auto& realm, auto& reader, JS::Value reason) {
                HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::mark_promise_as_handled(reader->cancel(reason));
            }));
        }
        HTML::TemporaryExecutionContext context { m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        failure_callback->function()(reason, failure);
    }

    void discard_partial_body()
    {
        if (!m_budget)
            return;
        m_budget->m_pending_reads.remove(m_pending_read);
        auto length = m_bytes.size();
        m_bytes.clear();
        m_budget->release(length);
        m_budget = nullptr;
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_realm);
        visitor.visit(m_reader);
        visitor.visit(m_success);
        visitor.visit(m_failure);
    }

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<Streams::ReadableStreamDefaultReader> m_reader;
    RefPtr<CacheBodyBudget> m_budget;
    GC::Ptr<CacheBodyReadSuccess> m_success;
    GC::Ptr<CacheBodyReadFailure> m_failure;
    ByteBuffer m_bytes;
    CacheBodyBudget::PendingRead m_pending_read;
};

GC_DEFINE_ALLOCATOR(CacheBodyReadRequest);

void CacheBodyBudget::discard()
{
    m_discarded = true;
    while (auto* pending = m_pending_reads.take_first()) {
        if (auto request = pending->request.ptr())
            request->discard();
    }
}

void read_cache_body(JS::Realm& realm, Streams::ReadableStreamDefaultReader& reader, NonnullRefPtr<CacheBodyBudget> budget, GC::Ref<CacheBodyReadSuccess> success, GC::Ref<CacheBodyReadFailure> failure)
{
    auto request = realm.heap().allocate<CacheBodyReadRequest>(realm, reader, move(budget), success, failure);
    request->start();
}

}
