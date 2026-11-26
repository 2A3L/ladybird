/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocolTypes.h>

namespace IPC {

using namespace Web::ServiceWorker::CacheStorageIPC;

template<>
ErrorOr<void> encode(Encoder& encoder, Name const& name)
{
    if (name.value.length_in_code_units() > maximum_name_length)
        return Error::from_string_literal("Cache Storage name exceeds the maximum length");
    return encoder.encode(name.value);
}

template<>
ErrorOr<Name> decode(Decoder& decoder)
{
    return Name { TRY(decoder.decode_with_max_size<Utf16String>(maximum_name_length)) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Snapshot const& snapshot)
{
    if (snapshot.bytes.size() > maximum_snapshot_size)
        return Error::from_string_literal("Cache Storage snapshot exceeds the maximum size");
    return encoder.encode(snapshot.bytes);
}

template<>
ErrorOr<Snapshot> decode(Decoder& decoder)
{
    return Snapshot { TRY(decoder.decode_with_max_size<ByteBuffer>(maximum_snapshot_size)) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Names const& names)
{
    if (names.names.size() > maximum_name_count)
        return Error::from_string_literal("Cache Storage list exceeds the maximum name count");
    return encoder.encode(names.names);
}

template<>
ErrorOr<Names> decode(Decoder& decoder)
{
    return Names { TRY(decoder.decode_with_max_size<Vector<Name>>(maximum_name_count)) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, CacheIds const& caches)
{
    if (caches.ids.size() > maximum_name_count)
        return Error::from_string_literal("Cache Storage enumeration exceeds the maximum cache count");
    return encoder.encode(caches.ids);
}

template<>
ErrorOr<CacheIds> decode(Decoder& decoder)
{
    return CacheIds { TRY(decoder.decode_with_max_size<Vector<CacheId>>(maximum_name_count)) };
}

}
