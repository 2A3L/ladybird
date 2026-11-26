/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/DistinctNumeric.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::ServiceWorker::CacheStorageIPC {

inline constexpr size_t maximum_name_length = 4096;
inline constexpr size_t maximum_name_count = 256;
inline constexpr size_t maximum_snapshot_size = 8 * MiB;

// Request IDs correlate replies. The Hub authorizes every cache lease against its owning environment.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, RequestId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CacheId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, Revision);

struct Name {
    Utf16String value;
};

struct Snapshot {
    ByteBuffer bytes;
};

struct Names {
    Vector<Name> names;
};

struct CacheIds {
    Vector<CacheId> ids;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ServiceWorker::CacheStorageIPC::Name const&);

template<>
WEBCOMMON_API ErrorOr<Web::ServiceWorker::CacheStorageIPC::Name> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ServiceWorker::CacheStorageIPC::Snapshot const&);

template<>
WEBCOMMON_API ErrorOr<Web::ServiceWorker::CacheStorageIPC::Snapshot> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ServiceWorker::CacheStorageIPC::Names const&);

template<>
WEBCOMMON_API ErrorOr<Web::ServiceWorker::CacheStorageIPC::Names> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ServiceWorker::CacheStorageIPC::CacheIds const&);

template<>
WEBCOMMON_API ErrorOr<Web::ServiceWorker::CacheStorageIPC::CacheIds> decode(Decoder&);

}
