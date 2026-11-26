/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/IntrusiveList.h>
#include <AK/RefCounted.h>
#include <LibGC/Function.h>
#include <LibGC/Weak.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::ServiceWorker {

class CacheBodyReadRequest;

inline constexpr size_t maximum_cache_body_bytes = 8 * MiB;

class WEB_API CacheBodyBudget final : public RefCounted<CacheBodyBudget> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    static NonnullRefPtr<CacheBodyBudget> create(size_t maximum_bytes);
    ~CacheBodyBudget();

    bool try_reserve(size_t bytes);
    void release(size_t bytes);

    // Drop partial bytes and cancel pending reads. Successful bodies stay charged to their owner.
    void discard();
    bool is_discarded() const { return m_discarded; }

private:
    friend class CacheBodyReadRequest;

    struct PendingRead {
        GC::Weak<CacheBodyReadRequest> request;
        IntrusiveListNode<PendingRead> node;
    };

    explicit CacheBodyBudget(size_t maximum_bytes)
        : m_maximum_bytes(maximum_bytes)
    {
    }

    size_t m_maximum_bytes { 0 };
    size_t m_reserved_bytes { 0 };
    bool m_discarded { false };
    IntrusiveList<&PendingRead::node> m_pending_reads;
};

enum class CacheBodyFailure {
    Stream,
    Quota,
};

using CacheBodyReadSuccess = GC::Function<void(ByteBuffer)>;
using CacheBodyReadFailure = GC::Function<void(JS::Value, CacheBodyFailure)>;

// Share one budget across the reads of an operation and keep it alive while the operation retains their results.
WEB_API void read_cache_body(JS::Realm&, Streams::ReadableStreamDefaultReader&, NonnullRefPtr<CacheBodyBudget>, GC::Ref<CacheBodyReadSuccess>, GC::Ref<CacheBodyReadFailure>);

}
