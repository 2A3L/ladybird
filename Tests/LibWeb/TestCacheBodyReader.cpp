/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibJS/Runtime/Realm.h>
#include <LibTest/TestCase.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/ServiceWorker/CacheBodyReader.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/Promise.h>

using namespace Web::ServiceWorker;

namespace {

struct ReadResult final : public RefCounted<ReadResult> {
    GC_HOLDS_NO_CELL_EDGES;
    Optional<ByteBuffer> bytes;
    Optional<CacheBodyFailure> failure;
    size_t failure_count { 0 };
    size_t cancel_count { 0 };

    bool finished() const { return bytes.has_value() || failure.has_value(); }
};

void start_read(JS::Realm& realm, Web::Streams::ReadableStream& stream, NonnullRefPtr<CacheBodyBudget> budget, NonnullRefPtr<ReadResult> result)
{
    auto reader = MUST(stream.get_a_reader());
    auto success = GC::create_function(realm.heap(), GC::captures(result), [](auto& result, ByteBuffer bytes) {
        result->bytes = move(bytes);
    });
    auto failure = GC::create_function(realm.heap(), GC::captures(result), [](auto& result, JS::Value, CacheBodyFailure failure) {
        result->failure = failure;
        ++result->failure_count;
    });
    read_cache_body(realm, *reader, move(budget), success, failure);
}

void run_pending_reads(Core::EventLoop& event_loop)
{
    for (unsigned i = 0; i < 8; ++i)
        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

}

TEST_CASE(completed_cache_body_keeps_its_exact_bytes_charged)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto& realm = create_test_document()->relevant_settings_object().realm();
    Web::HTML::TemporaryExecutionContext context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto budget = CacheBodyBudget::create(6);
    auto stream = Web::Streams::ReadableStream::create(realm);
    stream->set_up_with_byte_reading_support(realm);
    constexpr u8 first[] { 0, 0xff, 0x12 };
    constexpr u8 second[] { 0x80, 0 };
    MUST(stream->enqueue_bytes(first));
    MUST(stream->enqueue_bytes(second));
    stream->close();

    auto result = make_ref_counted<ReadResult>();
    start_read(realm, *stream, budget, result);
    run_pending_reads(event_loop);

    VERIFY(result->bytes.has_value());
    constexpr u8 expected[] { 0, 0xff, 0x12, 0x80, 0 };
    EXPECT_EQ(result->bytes->bytes(), ReadonlyBytes { expected });
    EXPECT(!budget->try_reserve(2));
    EXPECT(budget->try_reserve(1));
}

TEST_CASE(rejected_cache_body_releases_its_partial_bytes_and_cancels_its_stream)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto& realm = create_test_document()->relevant_settings_object().realm();
    Web::HTML::TemporaryExecutionContext context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto budget = CacheBodyBudget::create(5);
    auto result = make_ref_counted<ReadResult>();
    auto stream = Web::Streams::ReadableStream::create(realm);
    auto cancel = GC::create_function(realm.heap(), GC::captures(realm, result), [](auto& realm, auto& result, JS::Value) {
        ++result->cancel_count;
        // A tee branch's cancellation can remain pending until its sibling settles.
        return Web::WebIDL::create_promise(realm);
    });
    stream->set_up_with_byte_reading_support(realm, nullptr, cancel);
    MUST(stream->enqueue_bytes("abc"sv.bytes()));
    MUST(stream->enqueue_bytes("def"sv.bytes()));
    start_read(realm, *stream, budget, result);
    run_pending_reads(event_loop);

    EXPECT(result->finished());
    EXPECT(!result->bytes.has_value());
    EXPECT(result->failure == CacheBodyFailure::Quota);
    EXPECT_EQ(result->cancel_count, 1u);
    EXPECT(budget->try_reserve(5));
}

TEST_CASE(discard_cancels_a_quiet_cache_body_and_immediately_releases_partial_bytes)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto& realm = create_test_document()->relevant_settings_object().realm();
    Web::HTML::TemporaryExecutionContext context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto budget = CacheBodyBudget::create(5);
    auto result = make_ref_counted<ReadResult>();
    auto stream = Web::Streams::ReadableStream::create(realm);
    auto cancel = GC::create_function(realm.heap(), GC::captures(realm, result), [](auto& realm, auto& result, JS::Value) {
        ++result->cancel_count;
        return Web::WebIDL::create_promise(realm);
    });
    stream->set_up_with_byte_reading_support(realm, nullptr, cancel);
    MUST(stream->enqueue_bytes("abc"sv.bytes()));
    start_read(realm, *stream, budget, result);
    run_pending_reads(event_loop);
    EXPECT(!result->finished());

    auto first = CacheBodyBudget::create(maximum_cache_body_bytes);
    auto second = CacheBodyBudget::create(maximum_cache_body_bytes);
    EXPECT(first->try_reserve(maximum_cache_body_bytes));
    EXPECT(!second->try_reserve(maximum_cache_body_bytes));
    budget->discard();
    budget->discard();
    EXPECT(second->try_reserve(maximum_cache_body_bytes));

    run_pending_reads(event_loop);
    EXPECT(!result->bytes.has_value());
    EXPECT(result->failure == CacheBodyFailure::Stream);
    EXPECT_EQ(result->failure_count, 1u);
    EXPECT_EQ(result->cancel_count, 1u);
}

TEST_CASE(concurrent_cache_body_budgets_share_the_renderer_limit)
{
    auto first = CacheBodyBudget::create(maximum_cache_body_bytes);
    auto last = CacheBodyBudget::create(1);
    {
        auto second = CacheBodyBudget::create(maximum_cache_body_bytes);
        EXPECT(first->try_reserve(maximum_cache_body_bytes));
        EXPECT(second->try_reserve(maximum_cache_body_bytes));
        EXPECT(!last->try_reserve(1));
    }
    EXPECT(last->try_reserve(1));
    last->discard();
    EXPECT(!last->try_reserve(0));
}
