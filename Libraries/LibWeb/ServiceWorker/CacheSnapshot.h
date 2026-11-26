/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <LibWeb/Export.h>
#include <LibWeb/ServiceWorker/NameToCacheMap.h>

namespace Web::ServiceWorker {

inline constexpr size_t maximum_cache_snapshot_entries = 4096;

// The cache owner origin must be obtained from trusted browser state, never from the snapshot.
// Bodies that are not materialized as bytes and requests needing trusted provenance cannot be encoded.
WEB_API ErrorOr<ByteBuffer> encode_cache_snapshot(RequestResponseList const&, URL::Origin const& cache_owner_origin, size_t maximum_size);
WEB_API ErrorOr<GC::Ref<RequestResponseList>> decode_cache_snapshot(JS::Realm&, ReadonlyBytes, URL::Origin const& cache_owner_origin, size_t maximum_size);

}
