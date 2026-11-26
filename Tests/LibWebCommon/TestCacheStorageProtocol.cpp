/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocol.h>

using namespace Web::ServiceWorker::CacheStorageIPC;

template<typename T>
static ErrorOr<T> decode_message(IPC::MessageBuffer const& message)
{
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<T>();
}

template<typename T>
static T round_trip(T const& value)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(value));
    return MUST(decode_message<T>(message));
}

TEST_CASE(cache_names_preserve_utf16_code_units_and_order)
{
    auto name = u"cache\0\xD800z"_utf16;
    auto request = round_trip(Request { Open { Name { name } } });
    EXPECT_EQ(request.get<Open>().name.value, name);
    EXPECT_EQ(request.get<Open>().name.value.code_unit_at(5), 0);
    EXPECT_EQ(request.get<Open>().name.value.code_unit_at(6), 0xd800);

    Names names;
    names.names.append(Name { name });
    names.names.append(Name { u""_utf16 });
    auto reply = round_trip(Reply { move(names) });
    auto const& decoded = reply.get<Names>().names;
    VERIFY(decoded.size() == 2);
    EXPECT_EQ(decoded[0].value, name);
    EXPECT(decoded[1].value.is_empty());
}

TEST_CASE(cache_snapshot_round_trips_with_its_revision)
{
    constexpr u8 bytes[] { 0, 0xff, 0x12 };
    auto request = round_trip(Request { Commit {
        .id = CacheId { 21 },
        .expected_revision = Revision { 6 },
        .snapshot = Snapshot { MUST(ByteBuffer::copy(bytes)) },
    } });
    auto& operation = request.get<Commit>();
    EXPECT_EQ(operation.id.value(), 21u);
    EXPECT_EQ(operation.expected_revision.value(), 6u);
    EXPECT_EQ(operation.snapshot.bytes.bytes(), ReadonlyBytes { bytes });

    auto reply = round_trip(Reply { Loaded { Revision { 7 }, move(operation.snapshot) } });
    EXPECT_EQ(reply.get<Loaded>().revision.value(), 7u);
    EXPECT_EQ(reply.get<Loaded>().snapshot.bytes.bytes(), ReadonlyBytes { bytes });
}

TEST_CASE(cache_existence_queries_round_trip)
{
    auto request = round_trip(Request { Has { Name { u"cache"_utf16 } } });
    EXPECT_EQ(request.get<Has>().name.value, u"cache"_utf16);
    EXPECT(round_trip(Reply { Exists { true } }).get<Exists>().exists);
    EXPECT(!round_trip(Reply { Exists { false } }).get<Exists>().exists);
}

TEST_CASE(cache_enumeration_preserves_identity_order)
{
    EXPECT(round_trip(Request { Enumerate {} }).has<Enumerate>());
    auto reply = round_trip(Reply { CacheIds { { CacheId { 21 }, CacheId { 6 } } } });
    auto const& ids = reply.get<CacheIds>().ids;
    VERIFY(ids.size() == 2);
    EXPECT_EQ(ids[0].value(), 21u);
    EXPECT_EQ(ids[1].value(), 6u);
}

TEST_CASE(cache_wire_bounds_reject_declared_sizes_before_decoding_contents)
{
    IPC::MessageBuffer name_message;
    IPC::Encoder name_encoder { name_message };
    MUST(name_encoder.encode(false));
    MUST(name_encoder.encode_size(maximum_name_length + 1));
    auto name = decode_message<Name>(name_message);
    VERIFY(name.is_error());
    EXPECT_EQ(name.error().string_literal(), "IPC decode: Size exceeds the maximum"sv);

    IPC::MessageBuffer snapshot_message;
    IPC::Encoder snapshot_encoder { snapshot_message };
    MUST(snapshot_encoder.encode_size(maximum_snapshot_size + 1));
    auto snapshot = decode_message<Snapshot>(snapshot_message);
    VERIFY(snapshot.is_error());
    EXPECT_EQ(snapshot.error().string_literal(), "IPC decode: Size exceeds the maximum"sv);

    IPC::MessageBuffer names_message;
    IPC::Encoder names_encoder { names_message };
    MUST(names_encoder.encode_size(maximum_name_count + 1));
    auto names = decode_message<Names>(names_message);
    VERIFY(names.is_error());
    EXPECT_EQ(names.error().string_literal(), "IPC decode: Size exceeds the maximum"sv);

    IPC::MessageBuffer nested_name_message;
    IPC::Encoder nested_name_encoder { nested_name_message };
    MUST(nested_name_encoder.encode_size(1));
    MUST(nested_name_encoder.encode(false));
    MUST(nested_name_encoder.encode_size(maximum_name_length + 1));
    auto nested_name = decode_message<Names>(nested_name_message);
    VERIFY(nested_name.is_error());
    EXPECT_EQ(nested_name.error().string_literal(), "IPC decode: Size exceeds the maximum"sv);

    IPC::MessageBuffer ids_message;
    IPC::Encoder ids_encoder { ids_message };
    MUST(ids_encoder.encode_size(maximum_name_count + 1));
    auto ids = decode_message<CacheIds>(ids_message);
    VERIFY(ids.is_error());
    EXPECT_EQ(ids.error().string_literal(), "IPC decode: Size exceeds the maximum"sv);
}

TEST_CASE(cache_protocol_rejects_unknown_operations_and_states)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(u8 { 255 }));
    EXPECT(decode_message<Request>(message).is_error());
    EXPECT(decode_message<Reply>(message).is_error());
    EXPECT(decode_message<StorageError>(message).is_error());
}
