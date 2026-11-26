/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/Array.h>
#include <AK/Utf16View.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/ServiceWorker/CacheSnapshot.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocolTypes.h>
#include <errno.h>

namespace Web::ServiceWorker {

namespace {

using Fetch::Infrastructure::Request;
using Fetch::Infrastructure::Response;
using Fetch::Infrastructure::ResponseBody;
using Fetch::Infrastructure::ResponseView;

// The codes are part of the on-disk format; append new values rather than reordering these arrays.
static constexpr Array request_modes { Request::Mode::SameOrigin, Request::Mode::CORS, Request::Mode::NoCORS, Request::Mode::Navigate, Request::Mode::WebSocket };
static constexpr Array credentials_modes { Request::CredentialsMode::Omit, Request::CredentialsMode::SameOrigin, Request::CredentialsMode::Include };
static constexpr Array cache_modes { HTTP::CacheMode::Default, HTTP::CacheMode::NoStore, HTTP::CacheMode::Reload, HTTP::CacheMode::NoCache, HTTP::CacheMode::ForceCache, HTTP::CacheMode::OnlyIfCached };
static constexpr Array redirect_modes { Request::RedirectMode::Follow, Request::RedirectMode::Error, Request::RedirectMode::Manual };
static constexpr Array referrer_policies { ReferrerPolicy::ReferrerPolicy::EmptyString, ReferrerPolicy::ReferrerPolicy::NoReferrer, ReferrerPolicy::ReferrerPolicy::NoReferrerWhenDowngrade, ReferrerPolicy::ReferrerPolicy::SameOrigin, ReferrerPolicy::ReferrerPolicy::Origin, ReferrerPolicy::ReferrerPolicy::StrictOrigin, ReferrerPolicy::ReferrerPolicy::OriginWhenCrossOrigin, ReferrerPolicy::ReferrerPolicy::StrictOriginWhenCrossOrigin, ReferrerPolicy::ReferrerPolicy::UnsafeURL };
static constexpr Array destinations { Request::Destination::Audio, Request::Destination::AudioWorklet, Request::Destination::Document, Request::Destination::Embed, Request::Destination::Font, Request::Destination::Frame, Request::Destination::IFrame, Request::Destination::Image, Request::Destination::JSON, Request::Destination::Manifest, Request::Destination::Object, Request::Destination::PaintWorklet, Request::Destination::Report, Request::Destination::Script, Request::Destination::ServiceWorker, Request::Destination::SharedWorker, Request::Destination::Style, Request::Destination::Text, Request::Destination::Track, Request::Destination::Video, Request::Destination::WebIdentity, Request::Destination::Worker, Request::Destination::XSLT };
static constexpr Array redirect_taints { Fetch::Infrastructure::RedirectTaint::SameOrigin, Fetch::Infrastructure::RedirectTaint::SameSite, Fetch::Infrastructure::RedirectTaint::CrossSite };
static constexpr Array cache_states { Response::CacheState::Local, Response::CacheState::Validated };
static constexpr Array priorities { Request::Priority::High, Request::Priority::Low, Request::Priority::Auto };
static constexpr size_t max_components = 32768;
static constexpr size_t max_headers = 128;
static constexpr size_t max_url_count = 32;
static constexpr size_t max_metadata_size = 64 * KiB;
static constexpr u32 version = 1;

class Writer {
public:
    explicit Writer(size_t limit)
        : m_limit(min(limit, CacheStorageIPC::maximum_snapshot_size))
    {
    }

    ErrorOr<void> append(ReadonlyBytes bytes)
    {
        if (bytes.size() > m_limit - m_bytes.size())
            return Error::from_errno(E2BIG);
        return m_bytes.try_append(bytes);
    }
    ErrorOr<void> byte(u8 value) { return append({ &value, 1 }); }
    ErrorOr<void> number(u32 value)
    {
        u8 bytes[] { static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(value >> 16), static_cast<u8>(value >> 24) };
        return append(bytes);
    }
    ErrorOr<void> number64(u64 value)
    {
        TRY(number(static_cast<u32>(value)));
        return number(static_cast<u32>(value >> 32));
    }
    ErrorOr<void> blob(ReadonlyBytes value)
    {
        if (value.size() > CacheStorageIPC::maximum_snapshot_size)
            return Error::from_errno(E2BIG);
        TRY(number(static_cast<u32>(value.size())));
        return append(value);
    }
    ErrorOr<void> utf16(Utf16String const& value)
    {
        auto view = value.utf16_view();
        if (view.length_in_code_units() > CacheStorageIPC::maximum_snapshot_size / 2)
            return Error::from_errno(E2BIG);
        TRY(number(static_cast<u32>(view.length_in_code_units())));
        for (size_t i = 0; i < view.length_in_code_units(); ++i) {
            auto code_unit = view.code_unit_at(i);
            TRY(byte(static_cast<u8>(code_unit)));
            TRY(byte(static_cast<u8>(code_unit >> 8)));
        }
        return {};
    }
    ErrorOr<void> count(size_t count)
    {
        if (count > max_components - m_components)
            return Error::from_errno(E2BIG);
        m_components += count;
        return number(static_cast<u32>(count));
    }
    ByteBuffer release() { return move(m_bytes); }

private:
    size_t m_limit;
    size_t m_components { 0 };
    ByteBuffer m_bytes;
};

class Reader {
public:
    explicit Reader(ReadonlyBytes input)
        : m_input(input)
    {
    }
    ErrorOr<ReadonlyBytes> take(size_t size)
    {
        if (size > m_input.size())
            return Error::from_string_literal("Truncated cache snapshot");
        auto result = m_input.slice(0, size);
        m_input = m_input.slice(size);
        return result;
    }
    ErrorOr<u8> byte() { return TRY(take(1))[0]; }
    ErrorOr<u32> number()
    {
        auto bytes = TRY(take(4));
        return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 | static_cast<u32>(bytes[3]) << 24;
    }
    ErrorOr<u64> number64()
    {
        auto lo = TRY(number());
        auto hi = TRY(number());
        return static_cast<u64>(lo) | static_cast<u64>(hi) << 32;
    }
    ErrorOr<ReadonlyBytes> blob() { return take(TRY(number())); }
    ErrorOr<Utf16String> utf16()
    {
        auto length = TRY(number());
        if (length == 0)
            return Utf16String {};
        if (length > m_input.size() / 2)
            return Error::from_string_literal("Truncated cache snapshot UTF-16 string");
        Vector<char16_t> code_units;
        TRY(code_units.try_ensure_capacity(length));
        for (u32 i = 0; i < length; ++i) {
            auto lo = TRY(byte());
            auto hi = TRY(byte());
            code_units.unchecked_append(static_cast<char16_t>(lo | static_cast<u16>(hi) << 8));
        }
        return Utf16String::from_utf16({ code_units.data(), code_units.size() });
    }
    ErrorOr<size_t> count()
    {
        auto count = TRY(number());
        if (count > max_components - m_components)
            return Error::from_errno(E2BIG);
        if (count > m_input.size())
            return Error::from_string_literal("Truncated cache snapshot components");
        m_components += count;
        return count;
    }
    bool done() const { return m_input.is_empty(); }

private:
    ReadonlyBytes m_input;
    size_t m_components { 0 };
};

template<typename T, size_t N>
ErrorOr<void> write_enum(Writer& writer, T value, Array<T, N> const& values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] == value)
            return writer.byte(static_cast<u8>(i));
    }
    return Error::from_string_literal("Unsupported cache snapshot enum value");
}

template<typename T, size_t N>
ErrorOr<T> read_enum(Reader& reader, Array<T, N> const& values)
{
    auto index = TRY(reader.byte());
    if (index >= values.size())
        return Error::from_string_literal("Invalid cache snapshot enum value");
    return values[index];
}

ErrorOr<void> write_headers(Writer& writer, HTTP::HeaderList const& headers)
{
    if (headers.headers().size() > max_headers)
        return Error::from_errno(E2BIG);
    TRY(writer.count(headers.headers().size()));
    for (auto const& header : headers) {
        if (header.name.length() > max_metadata_size || header.value.length() > max_metadata_size)
            return Error::from_errno(E2BIG);
        if (!HTTP::is_header_name(header.name) || !HTTP::is_header_value(header.value))
            return Error::from_string_literal("Invalid cache snapshot header");
        TRY(writer.blob(header.name.bytes()));
        TRY(writer.blob(header.value.bytes()));
    }
    return {};
}

ErrorOr<NonnullRefPtr<HTTP::HeaderList>> read_headers(Reader& reader)
{
    auto count = TRY(reader.count());
    if (count > max_headers)
        return Error::from_errno(E2BIG);
    Vector<HTTP::Header> headers;
    TRY(headers.try_ensure_capacity(count));
    for (size_t i = 0; i < count; ++i) {
        auto name = TRY(reader.blob());
        auto value = TRY(reader.blob());
        if (name.size() > max_metadata_size || value.size() > max_metadata_size)
            return Error::from_errno(E2BIG);
        StringView name_view { name };
        StringView value_view { value };
        if (!HTTP::is_header_name(name_view) || !HTTP::is_header_value(value_view))
            return Error::from_string_literal("Invalid cache snapshot header");
        headers.unchecked_append({ ByteString { name }, ByteString { value } });
    }
    return HTTP::HeaderList::create(move(headers));
}

ErrorOr<void> write_urls(Writer& writer, Vector<URL::URL> const& urls)
{
    if (urls.size() > max_url_count)
        return Error::from_errno(E2BIG);
    TRY(writer.count(urls.size()));
    for (auto const& url : urls) {
        if (url.serialize().bytes().size() > max_metadata_size)
            return Error::from_errno(E2BIG);
        TRY(writer.blob(url.serialize().bytes()));
    }
    return {};
}

ErrorOr<Vector<URL::URL>> read_urls(Reader& reader)
{
    auto count = TRY(reader.count());
    if (count > max_url_count)
        return Error::from_errno(E2BIG);
    Vector<URL::URL> urls;
    TRY(urls.try_ensure_capacity(count));
    for (size_t i = 0; i < count; ++i) {
        auto serialized = TRY(reader.blob());
        if (serialized.size() > max_metadata_size)
            return Error::from_errno(E2BIG);
        auto url = URL::Parser::basic_parse(StringView { serialized });
        if (!url.has_value() || url->serialize().bytes() != serialized)
            return Error::from_string_literal("Invalid cache snapshot URL");
        urls.unchecked_append(url.release_value());
    }
    return urls;
}

ErrorOr<bool> read_bool(Reader& reader)
{
    auto value = TRY(reader.byte());
    if (value > 1)
        return Error::from_string_literal("Invalid cache snapshot boolean");
    return value == 1;
}

bool valid_status_message(ReadonlyBytes bytes)
{
    return all_of(bytes, [](u8 byte) { return byte == '\t' || byte == ' ' || (byte >= 0x21 && byte <= 0x7e) || byte >= 0x80; });
}

bool safe_request_headers(HTTP::HeaderList const& headers, Request::Mode mode)
{
    for (auto const& header : headers) {
        if (HTTP::is_forbidden_request_header(header))
            return false;
        if (mode == Request::Mode::NoCORS) {
            auto combined = headers.get(header.name);
            if (!combined.has_value() || !Fetch::Infrastructure::is_no_cors_safelisted_request_header({ header.name, *combined }))
                return false;
        }
    }
    return true;
}

bool safe_referrer(URL::URL const& url, URL::Origin const& cache_owner_origin)
{
    return !cache_owner_origin.is_opaque()
        && url.scheme().is_one_of("http"sv, "https"sv)
        && url.username().is_empty()
        && url.password().is_empty()
        && url.serialize().bytes().size() <= max_metadata_size
        && url.origin().is_same_origin(cache_owner_origin);
}

ErrorOr<void> write_request(Writer& writer, Request const& request, URL::Origin const& cache_owner_origin)
{
    if (request.method() != "GET"sv || !request.body().has<Empty>() || request.url_list().is_empty())
        return Error::from_string_literal("Cache snapshot request must be a bodyless GET");
    if (request.mode() == Request::Mode::Navigate || request.mode() == Request::Mode::WebSocket || request.destination().has_value() || request.reload_navigation() || request.history_navigation() || !safe_request_headers(*request.header_list(), request.mode()))
        return Error::from_string_literal("Cache snapshot request requires trusted provenance");
    for (auto const& url : request.url_list()) {
        if (!url.scheme().is_one_of("http"sv, "https"sv) || !url.username().is_empty() || !url.password().is_empty())
            return Error::from_string_literal("Cache snapshot request URL must be HTTP(S)");
    }
    TRY(write_urls(writer, request.url_list()));
    TRY(write_headers(writer, *request.header_list()));
    TRY(write_enum(writer, request.mode(), request_modes));
    TRY(write_enum(writer, request.credentials_mode(), credentials_modes));
    TRY(write_enum(writer, request.cache_mode(), cache_modes));
    TRY(write_enum(writer, request.redirect_mode(), redirect_modes));
    TRY(write_enum(writer, request.referrer_policy(), referrer_policies));
    TRY(write_enum(writer, request.priority(), priorities));
    TRY(writer.byte(request.destination().has_value()));
    if (request.destination().has_value())
        TRY(write_enum(writer, *request.destination(), destinations));
    if (auto const* url = request.referrer().get_pointer<URL::URL>()) {
        if (url->serialize().bytes().size() > max_metadata_size)
            return Error::from_errno(E2BIG);
        if (!safe_referrer(*url, cache_owner_origin))
            return Error::from_string_literal("Cache snapshot referrer does not belong to its owner");
        TRY(writer.byte(2));
        TRY(writer.blob(url->serialize().bytes()));
    } else {
        auto referrer = request.referrer().get<Request::Referrer>();
        if (referrer != Request::Referrer::Client && referrer != Request::Referrer::NoReferrer)
            return Error::from_string_literal("Unsupported cache snapshot referrer");
        TRY(writer.byte(referrer == Request::Referrer::Client ? 0 : 1));
    }
    TRY(writer.utf16(request.integrity_metadata()));
    TRY(writer.byte(request.keepalive()));
    TRY(writer.byte(request.reload_navigation()));
    TRY(writer.byte(request.history_navigation()));
    return {};
}

ErrorOr<NonnullRefPtr<Request>> read_request(Reader& reader, URL::Origin const& cache_owner_origin)
{
    auto urls = TRY(read_urls(reader));
    if (urls.is_empty())
        return Error::from_string_literal("Cache snapshot request has no URL");
    for (auto const& url : urls) {
        if (!url.scheme().is_one_of("http"sv, "https"sv) || !url.username().is_empty() || !url.password().is_empty())
            return Error::from_string_literal("Cache snapshot request URL must be HTTP(S)");
    }
    auto headers = TRY(read_headers(reader));
    auto mode = TRY(read_enum(reader, request_modes));
    auto credentials = TRY(read_enum(reader, credentials_modes));
    auto cache_mode = TRY(read_enum(reader, cache_modes));
    auto redirect = TRY(read_enum(reader, redirect_modes));
    auto referrer_policy = TRY(read_enum(reader, referrer_policies));
    auto priority = TRY(read_enum(reader, priorities));
    Optional<Request::Destination> destination;
    if (TRY(read_bool(reader)))
        destination = TRY(read_enum(reader, destinations));
    auto referrer_tag = TRY(reader.byte());
    Request::ReferrerType referrer = Request::Referrer::Client;
    if (referrer_tag == 1) {
        referrer = Request::Referrer::NoReferrer;
    } else if (referrer_tag == 2) {
        auto serialized = TRY(reader.blob());
        if (serialized.size() > max_metadata_size)
            return Error::from_errno(E2BIG);
        auto url = URL::Parser::basic_parse(StringView { serialized });
        if (!url.has_value() || url->serialize().bytes() != serialized || !safe_referrer(*url, cache_owner_origin))
            return Error::from_string_literal("Invalid cache snapshot referrer");
        referrer = url.release_value();
    } else if (referrer_tag != 0) {
        return Error::from_string_literal("Invalid cache snapshot referrer tag");
    }
    auto integrity = TRY(reader.utf16());
    auto keepalive = TRY(read_bool(reader));
    auto reload = TRY(read_bool(reader));
    auto history = TRY(read_bool(reader));
    if (mode == Request::Mode::Navigate || mode == Request::Mode::WebSocket || destination.has_value() || reload || history || !safe_request_headers(*headers, mode))
        return Error::from_string_literal("Cache snapshot request requires trusted provenance");
    auto request = Request::create();
    request->set_method("GET"sv);
    request->set_url_list(move(urls));
    request->set_header_list(move(headers));
    request->set_mode(mode);
    request->set_credentials_mode(credentials);
    request->set_cache_mode(cache_mode);
    request->set_redirect_mode(redirect);
    request->set_referrer_policy(referrer_policy);
    request->set_priority(priority);
    request->set_destination(destination);
    request->set_referrer(move(referrer));
    request->set_integrity_metadata(move(integrity));
    request->set_keepalive(keepalive);
    request->set_reload_navigation(reload);
    request->set_history_navigation(history);
    return request;
}

ErrorOr<void> write_response(Writer& writer, ResponseView const& view)
{
    auto const& record = *view.record();
    auto internal = record.unsafe_response();
    if (internal->aborted())
        return Error::from_string_literal("Unsupported cache snapshot response");
    u8 filter = 0;
    if (is<Fetch::Infrastructure::BasicFilteredResponse>(record))
        filter = 1;
    else if (is<Fetch::Infrastructure::CORSFilteredResponse>(record))
        filter = 2;
    else if (is<Fetch::Infrastructure::OpaqueFilteredResponse>(record))
        filter = 3;
    else if (is<Fetch::Infrastructure::OpaqueRedirectFilteredResponse>(record))
        filter = 4;
    else if (record.type() == Response::Type::Error && !is<Fetch::Infrastructure::FilteredResponse>(record))
        filter = 5;
    else if (is<Fetch::Infrastructure::FilteredResponse>(record) || record.type() != Response::Type::Default)
        return Error::from_string_literal("Unsupported cache snapshot response filter");
    if (filter != 5 && internal->type() != Response::Type::Default)
        return Error::from_string_literal("Unsupported cache snapshot response");
    if ((filter == 5 && (internal->status() != 0 || !internal->url_list().is_empty() || !internal->header_list()->is_empty() || !internal->status_message().is_empty() || internal->body().has_value() || !internal->cors_exposed_header_name_list().is_empty() || internal->range_requested() || !internal->request_includes_credentials() || internal->timing_allow_passed() || internal->redirect_taint() != Fetch::Infrastructure::RedirectTaint::SameOrigin || internal->cache_state().has_value() || internal->body_info() != Response::BodyInfo {} || !internal->navigation_timing_allow_values_list().is_empty()))
        || (filter != 5 && (internal->status() < 100 || internal->status() > 599 || (filter < 3 && internal->status() == 206))))
        return Error::from_string_literal("Invalid cache snapshot response status");
    if (filter == 4 && !Fetch::Infrastructure::is_redirect_status(internal->status()))
        return Error::from_string_literal("Invalid opaque redirect response status");
    if (internal->status_message().length() > max_metadata_size)
        return Error::from_errno(E2BIG);
    if (!valid_status_message(internal->status_message().bytes()))
        return Error::from_string_literal("Invalid cache snapshot status message");
    auto body = internal->body();
    if (body.has_value() && Fetch::Infrastructure::is_null_body_status(internal->status()))
        return Error::from_string_literal("Cache snapshot has a body for a null-body status");
    if (body.has_value() && !body->source().has_value())
        return Error::from_string_literal("Cache snapshot body is not materialized");
    if (filter == 0) {
        for (auto const& header : *internal->header_list()) {
            if (HTTP::is_forbidden_response_header_name(header.name))
                return Error::from_string_literal("Cache snapshot contains a forbidden response header");
        }
    }

    TRY(writer.byte(filter));
    TRY(writer.number(internal->status()));
    TRY(writer.blob(internal->status_message().bytes()));
    TRY(write_urls(writer, internal->url_list()));
    TRY(write_headers(writer, *internal->header_list()));
    if (internal->cors_exposed_header_name_list().size() > max_headers)
        return Error::from_errno(E2BIG);
    TRY(writer.count(internal->cors_exposed_header_name_list().size()));
    for (auto const& name : internal->cors_exposed_header_name_list()) {
        if (name.length() > max_metadata_size)
            return Error::from_errno(E2BIG);
        if (!HTTP::is_header_name(name))
            return Error::from_string_literal("Invalid CORS-exposed header name");
        TRY(writer.blob(name.bytes()));
    }
    TRY(writer.byte(internal->range_requested()));
    TRY(writer.byte(internal->request_includes_credentials()));
    TRY(writer.byte(internal->timing_allow_passed()));
    TRY(write_enum(writer, internal->redirect_taint(), redirect_taints));
    TRY(writer.byte(internal->cache_state().has_value()));
    if (internal->cache_state().has_value())
        TRY(write_enum(writer, *internal->cache_state(), cache_states));
    auto const& body_info = internal->body_info();
    TRY(writer.number64(body_info.encoded_size));
    TRY(writer.number64(body_info.decoded_size));
    TRY(writer.utf16(body_info.content_type));
    TRY(writer.count(internal->navigation_timing_allow_values_list().size()));
    for (auto const& values : internal->navigation_timing_allow_values_list()) {
        TRY(writer.count(values.size()));
        for (auto const& value : values)
            TRY(writer.blob(value.bytes()));
    }
    TRY(writer.byte(body.has_value()));
    if (body.has_value())
        TRY(writer.blob(body->source()->bytes()));
    return {};
}

ErrorOr<GC::Ref<ResponseView>> read_response(JS::Realm& realm, Reader& reader)
{
    auto filter = TRY(reader.byte());
    if (filter > 5)
        return Error::from_string_literal("Invalid cache snapshot response filter");
    auto status = TRY(reader.number());
    if ((filter == 5 && status != 0) || (filter != 5 && (status < 100 || status > 599 || (filter < 3 && status == 206))))
        return Error::from_string_literal("Invalid cache snapshot response status");
    if (filter == 4 && !Fetch::Infrastructure::is_redirect_status(status))
        return Error::from_string_literal("Invalid opaque redirect response status");
    auto message_bytes = TRY(reader.blob());
    if (message_bytes.size() > max_metadata_size || !valid_status_message(message_bytes))
        return Error::from_string_literal("Invalid cache snapshot status message");
    auto urls = TRY(read_urls(reader));
    auto headers = TRY(read_headers(reader));
    if (filter == 5 && (!message_bytes.is_empty() || !urls.is_empty() || !headers->is_empty()))
        return Error::from_string_literal("Invalid cache snapshot network error response");
    auto exposed_count = TRY(reader.count());
    if (exposed_count > max_headers)
        return Error::from_errno(E2BIG);
    Vector<ByteString> exposed;
    TRY(exposed.try_ensure_capacity(exposed_count));
    for (size_t i = 0; i < exposed_count; ++i) {
        auto name = TRY(reader.blob());
        if (name.size() > max_metadata_size || !HTTP::is_header_name(StringView { name }))
            return Error::from_string_literal("Invalid CORS-exposed header name");
        exposed.unchecked_append(ByteString { name });
    }
    auto range_requested = TRY(read_bool(reader));
    auto includes_credentials = TRY(read_bool(reader));
    auto timing_allowed = TRY(read_bool(reader));
    auto redirect_taint = TRY(read_enum(reader, redirect_taints));
    Optional<Response::CacheState> cache_state;
    if (TRY(read_bool(reader)))
        cache_state = TRY(read_enum(reader, cache_states));
    Response::BodyInfo body_info;
    body_info.encoded_size = TRY(reader.number64());
    body_info.decoded_size = TRY(reader.number64());
    body_info.content_type = TRY(reader.utf16());
    auto timing_count = TRY(reader.count());
    Vector<Vector<String>> timing_values;
    TRY(timing_values.try_ensure_capacity(timing_count));
    for (size_t i = 0; i < timing_count; ++i) {
        auto count = TRY(reader.count());
        Vector<String> values;
        TRY(values.try_ensure_capacity(count));
        for (size_t j = 0; j < count; ++j)
            values.unchecked_append(TRY(String::from_utf8(StringView { TRY(reader.blob()) })));
        timing_values.unchecked_append(move(values));
    }
    if (filter == 5 && (!exposed.is_empty() || range_requested || !includes_credentials || timing_allowed || redirect_taint != Fetch::Infrastructure::RedirectTaint::SameOrigin || cache_state.has_value() || body_info != Response::BodyInfo {} || !timing_values.is_empty()))
        return Error::from_string_literal("Invalid cache snapshot network error response");
    Optional<ResponseBody> body;
    if (TRY(read_bool(reader))) {
        if (filter == 5 || Fetch::Infrastructure::is_null_body_status(status))
            return Error::from_string_literal("Cache snapshot has a body for a null-body status");
        body = ResponseBody::from_bytes(Core::ImmutableBytes::adopt(TRY(ByteBuffer::copy(TRY(reader.blob())))));
    }

    if (filter == 0) {
        for (auto const& header : *headers) {
            if (HTTP::is_forbidden_response_header_name(header.name))
                return Error::from_string_literal("Cache snapshot contains a forbidden response header");
        }
    }
    auto response = Response::create();
    if (filter == 5)
        response->set_type(Response::Type::Error);
    response->set_status(status);
    response->set_status_message(ByteString { message_bytes });
    response->set_url_list(move(urls));
    response->set_header_list(move(headers));
    response->set_cors_exposed_header_name_list(move(exposed));
    response->set_range_requested(range_requested);
    response->set_request_includes_credentials(includes_credentials);
    response->set_timing_allow_passed(timing_allowed);
    response->set_redirect_taint(redirect_taint);
    response->set_cache_state(cache_state);
    response->set_body_info(move(body_info));
    response->set_navigation_timing_allow_values_list(move(timing_values));
    response->set_body(move(body));
    NonnullRefPtr<Response> filtered = response;
    switch (filter) {
    case 1:
        filtered = Fetch::Infrastructure::BasicFilteredResponse::create(response);
        break;
    case 2:
        filtered = Fetch::Infrastructure::CORSFilteredResponse::create(response);
        break;
    case 3:
        filtered = Fetch::Infrastructure::OpaqueFilteredResponse::create(response);
        break;
    case 4:
        filtered = Fetch::Infrastructure::OpaqueRedirectFilteredResponse::create(response);
        break;
    }
    return ResponseView::create(realm, move(filtered));
}

}

ErrorOr<ByteBuffer> encode_cache_snapshot(RequestResponseList const& entries, URL::Origin const& cache_owner_origin, size_t maximum_size)
{
    if (cache_owner_origin.is_opaque())
        return Error::from_string_literal("Cache snapshot owner origin is opaque");
    if (entries.elements().size() > maximum_cache_snapshot_entries)
        return Error::from_errno(E2BIG);
    Writer writer { maximum_size };
    TRY(writer.append("LBCS"sv.bytes()));
    TRY(writer.number(version));
    TRY(writer.count(entries.elements().size()));
    for (auto const& entry : entries.elements()) {
        TRY(write_request(writer, *entry->request, cache_owner_origin));
        TRY(write_response(writer, *entry->response));
    }
    return writer.release();
}

ErrorOr<GC::Ref<RequestResponseList>> decode_cache_snapshot(JS::Realm& realm, ReadonlyBytes bytes, URL::Origin const& cache_owner_origin, size_t maximum_size)
{
    if (cache_owner_origin.is_opaque())
        return Error::from_string_literal("Cache snapshot owner origin is opaque");
    if (bytes.size() > min(maximum_size, CacheStorageIPC::maximum_snapshot_size))
        return Error::from_errno(E2BIG);
    Reader reader { bytes };
    if (TRY(reader.take(4)) != "LBCS"sv.bytes() || TRY(reader.number()) != version)
        return Error::from_string_literal("Unknown cache snapshot format");
    auto count = TRY(reader.count());
    if (count > maximum_cache_snapshot_entries)
        return Error::from_errno(E2BIG);
    auto entries = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    TRY(entries->elements().try_ensure_capacity(count));
    for (size_t i = 0; i < count; ++i) {
        auto request = TRY(read_request(reader, cache_owner_origin));
        auto response = GC::make_root(TRY(read_response(realm, reader)));
        entries->elements().unchecked_append(GC::Heap::the().allocate<RequestResponse>(move(request), *response));
    }
    if (!reader.done())
        return Error::from_string_literal("Cache snapshot has trailing bytes");
    return GC::Ref { *entries };
}

}
