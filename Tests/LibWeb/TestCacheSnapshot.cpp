/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/ServiceWorker/CacheSnapshot.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocolTypes.h>
#include <errno.h>

using namespace Web;
using namespace Web::Fetch::Infrastructure;
using namespace Web::ServiceWorker;

static NonnullRefPtr<Request> make_request()
{
    auto request = Request::create();
    request->set_url(URL::Parser::basic_parse("https://example.test/file?query#fragment"sv).release_value());
    request->set_header_list(HTTP::HeaderList::create({ { "Accept"sv, "application/json"sv }, { "X-Example"sv, "value"sv } }));
    request->set_mode(Request::Mode::CORS);
    request->set_credentials_mode(Request::CredentialsMode::Include);
    request->set_cache_mode(HTTP::CacheMode::NoStore);
    request->set_redirect_mode(Request::RedirectMode::Manual);
    request->set_referrer(Request::Referrer::NoReferrer);
    request->set_referrer_policy(ReferrerPolicy::ReferrerPolicy::Origin);
    char16_t integrity[] { 'a', 0, 0xd800, 'b' };
    request->set_integrity_metadata(Utf16String::from_utf16({ integrity, 4 }));
    request->set_keepalive(true);
    return request;
}

static NonnullRefPtr<Response> make_response()
{
    auto response = Response::create();
    response->set_status(201);
    response->set_status_message(" Created "sv);
    response->set_url_list({ URL::Parser::basic_parse("https://other.test/path"sv).release_value() });
    response->set_header_list(HTTP::HeaderList::create({ { "X-Visible"sv, "visible"sv }, { "Set-Cookie"sv, "secret=value"sv }, { "X-Hidden"sv, "hidden"sv } }));
    response->set_cors_exposed_header_name_list({ "X-Visible"sv, "Set-Cookie"sv });
    response->set_body(ResponseBody::from_bytes(Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("data"sv.bytes())))));
    return response;
}

TEST_CASE(cache_snapshot_roundtrips_filters_and_metadata)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto cache_owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    auto request = make_request();
    auto cors = GC::make_root(ResponseView::create(realm, CORSFilteredResponse::create(make_response())));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *cors));
    auto opaque = GC::make_root(ResponseView::create(realm, OpaqueFilteredResponse::create(make_response())));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *opaque));
    auto redirect = make_response();
    redirect->set_status(302);
    auto opaque_redirect = GC::make_root(ResponseView::create(realm, OpaqueRedirectFilteredResponse::create(redirect)));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *opaque_redirect));
    auto no_body = make_response();
    no_body->header_list()->delete_("Set-Cookie"sv);
    no_body->set_body({});
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, ResponseView::create(realm, no_body)));
    auto empty_body = make_response();
    empty_body->header_list()->delete_("Set-Cookie"sv);
    empty_body->set_body(ResponseBody::from_bytes(Core::ImmutableBytes::adopt(ByteBuffer {})));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, ResponseView::create(realm, empty_body)));
    auto error = GC::make_root(ResponseView::create(realm, Response::network_error("synthetic error"_string)));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *error));

    auto encoded = MUST(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size));
    auto decoded = GC::make_root(MUST(decode_cache_snapshot(realm, encoded.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size)));
    EXPECT_EQ(decoded->elements().size(), 6u);
    auto& result_request = *decoded->elements()[0]->request;
    EXPECT_EQ(result_request.method(), "GET"sv);
    EXPECT_EQ(result_request.url().serialize(), request->url().serialize());
    EXPECT_EQ(result_request.integrity_metadata(), request->integrity_metadata());
    EXPECT(result_request.credentials_mode() == Request::CredentialsMode::Include);
    EXPECT_EQ(result_request.header_list()->get("X-Example"sv).value(), "value"sv);
    auto& cors_result = *decoded->elements()[0]->response;
    EXPECT(cors_result.type() == Response::Type::CORS);
    EXPECT_EQ(cors_result.status_message(), " Created "sv);
    EXPECT(cors_result.header_list()->contains("X-Visible"sv));
    EXPECT(!cors_result.header_list()->contains("X-Hidden"sv));
    EXPECT(!cors_result.header_list()->contains("Set-Cookie"sv));
    for (size_t index : { 1u, 2u }) {
        auto& result = *decoded->elements()[index]->response;
        EXPECT(result.type() == (index == 1 ? Response::Type::Opaque : Response::Type::OpaqueRedirect));
        EXPECT_EQ(result.status(), 0);
        EXPECT(result.header_list()->is_empty());
        EXPECT(!result.body());
        EXPECT(result.record()->unsafe_response()->body()->source()->bytes() == "data"sv.bytes());
    }
    EXPECT(!decoded->elements()[3]->response->record()->unsafe_response()->body().has_value());
    EXPECT(decoded->elements()[4]->response->record()->unsafe_response()->body().has_value());
    EXPECT(decoded->elements()[4]->response->record()->unsafe_response()->body()->source()->is_empty());
    EXPECT(decoded->elements()[5]->response->type() == Response::Type::Error);
    EXPECT_EQ(decoded->elements()[5]->response->status(), 0);
    EXPECT(decoded->elements()[5]->response->header_list()->is_empty());
    EXPECT(!decoded->elements()[5]->response->body());
    EXPECT_EQ(MUST(encode_cache_snapshot(*decoded, cache_owner, CacheStorageIPC::maximum_snapshot_size)), encoded);
}

TEST_CASE(cache_snapshot_rejects_corruption_and_limits)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto cache_owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    auto internal = make_response();
    internal->header_list()->delete_("Set-Cookie"sv);
    auto response = GC::make_root(ResponseView::create(realm, internal));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(make_request(), *response));
    auto encoded = MUST(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size));
    for (size_t length = 0; length < encoded.size(); ++length)
        EXPECT(decode_cache_snapshot(realm, encoded.bytes().slice(0, length), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto bad_version = encoded;
    bad_version[4] = 2;
    EXPECT(decode_cache_snapshot(realm, bad_version.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto trailing = encoded;
    MUST(trailing.try_append(0));
    EXPECT(decode_cache_snapshot(realm, trailing.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto excessive_count = encoded;
    for (size_t i = 8; i < 12; ++i)
        excessive_count[i] = 0xff;
    EXPECT(decode_cache_snapshot(realm, excessive_count.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto oversized_url = encoded;
    for (size_t i = 16; i < 20; ++i)
        oversized_url[i] = 0xff;
    EXPECT(decode_cache_snapshot(realm, oversized_url.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto invalid_body_tag = encoded;
    auto body_marker = "data"sv.bytes();
    bool found_body = false;
    for (size_t i = 5; i + body_marker.size() <= invalid_body_tag.size(); ++i) {
        if (invalid_body_tag.bytes().slice(i, body_marker.size()) == body_marker) {
            invalid_body_tag[i - 5] = 2;
            found_body = true;
            break;
        }
    }
    EXPECT(found_body);
    EXPECT(decode_cache_snapshot(realm, invalid_body_tag.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto invalid_filter = encoded;
    auto status_marker = " Created "sv.bytes();
    bool found_status = false;
    for (size_t i = 9; i + status_marker.size() <= invalid_filter.size(); ++i) {
        if (invalid_filter.bytes().slice(i, status_marker.size()) == status_marker) {
            invalid_filter[i - 9] = 255;
            found_status = true;
            break;
        }
    }
    EXPECT(found_status);
    EXPECT(decode_cache_snapshot(realm, invalid_filter.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    EXPECT(encode_cache_snapshot(*list, cache_owner, 4).is_error());
    EXPECT(decode_cache_snapshot(realm, encoded.bytes(), cache_owner, 4).is_error());
}

TEST_CASE(cache_snapshot_rejects_unmaterialized_opaque_body)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto cache_owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto internal = make_response();
    internal->set_body(ResponseBody { ResponseBody::ReadThroughStream {}, {} });
    auto opaque = OpaqueFilteredResponse::create(internal);
    auto body = Body::create_over_bytes(realm, Core::ImmutableBytes::adopt(MUST(ByteBuffer::copy("hidden"sv.bytes()))));
    auto view = GC::make_root(ResponseView::create(opaque, body));
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(make_request(), *view));
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
}

TEST_CASE(cache_snapshot_rejects_untrusted_request_and_response_metadata)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto cache_owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    auto request = make_request();
    auto internal = make_response();
    internal->header_list()->delete_("Set-Cookie"sv);
    auto response = GC::make_root(ResponseView::create(realm, internal));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *response));
    auto valid = MUST(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size));

    auto forbidden_name = valid;
    auto marker = "X-Example"sv.bytes();
    bool changed = false;
    for (size_t i = 0; i + marker.size() <= forbidden_name.size(); ++i) {
        if (forbidden_name.bytes().slice(i, marker.size()) == marker) {
            auto replacement = "Proxy-xxx"sv.bytes();
            for (size_t j = 0; j < replacement.size(); ++j)
                forbidden_name[i + j] = replacement[j];
            changed = true;
            break;
        }
    }
    EXPECT(changed);
    EXPECT(decode_cache_snapshot(realm, forbidden_name.bytes(), cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());

    request->set_header_list(HTTP::HeaderList::create({ { "Cookie"sv, "redacted"sv } }));
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    request->set_header_list(HTTP::HeaderList::create());
    request->set_mode(Request::Mode::NoCORS);
    request->set_header_list(HTTP::HeaderList::create({ { "Accept"sv, ByteString::repeated('a', 80) }, { "Accept"sv, ByteString::repeated('b', 80) } }));
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    request->set_header_list(HTTP::HeaderList::create());
    request->set_mode(Request::Mode::Navigate);
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    request->set_mode(Request::Mode::CORS);
    request->set_referrer(URL::Parser::basic_parse("https://other.test/"sv).release_value());
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    request->set_referrer(Request::Referrer::Client);
    request->set_url(URL::Parser::basic_parse("https://user:pass@example.test/"sv).release_value());
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    request->set_url(URL::Parser::basic_parse("https://example.test/"sv).release_value());
    internal->header_list()->append({ "Set-Cookie"sv, "redacted"sv });
    EXPECT(encode_cache_snapshot(*list, cache_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
}

TEST_CASE(cache_snapshot_referrer_is_bound_to_its_trusted_owner)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    auto request = make_request();
    request->set_referrer(URL::Parser::basic_parse("https://owner.example:8443/path?query=one#fragment"sv).release_value());
    auto internal = make_response();
    internal->header_list()->delete_("Set-Cookie"sv);
    auto response = GC::make_root(ResponseView::create(realm, internal));
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *response));

    auto encoded = MUST(encode_cache_snapshot(*list, owner, CacheStorageIPC::maximum_snapshot_size));
    auto decoded = GC::make_root(MUST(decode_cache_snapshot(realm, encoded.bytes(), owner, CacheStorageIPC::maximum_snapshot_size)));
    auto const& decoded_request = *decoded->elements()[0]->request;
    EXPECT_EQ(decoded_request.url().serialize(), "https://example.test/file?query#fragment"sv);
    EXPECT(decoded_request.referrer().has<URL::URL>());
    EXPECT_EQ(decoded_request.referrer().get<URL::URL>().serialize(), "https://owner.example:8443/path?query=one#fragment"sv);

    for (auto wrong_owner_url : { "http://owner.example:8443/"sv, "https://other.example:8443/"sv, "https://owner.example:8444/"sv }) {
        auto wrong_owner = URL::Parser::basic_parse(wrong_owner_url)->origin();
        EXPECT(decode_cache_snapshot(realm, encoded.bytes(), wrong_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
        EXPECT(encode_cache_snapshot(*list, wrong_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    }
    auto opaque_owner = URL::Origin::create_opaque();
    EXPECT(decode_cache_snapshot(realm, encoded.bytes(), opaque_owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    EXPECT(encode_cache_snapshot(*list, opaque_owner, CacheStorageIPC::maximum_snapshot_size).is_error());

    auto corrupt_owner = encoded;
    auto noncanonical_owner = encoded;
    auto marker = "owner.example"sv.bytes();
    bool found = false;
    for (size_t i = 0; i + marker.size() <= encoded.size(); ++i) {
        if (encoded.bytes().slice(i, marker.size()) == marker) {
            corrupt_owner[i] = 'x';
            noncanonical_owner[i] = 'O';
            found = true;
            break;
        }
    }
    EXPECT(found);
    EXPECT(decode_cache_snapshot(realm, corrupt_owner.bytes(), owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    EXPECT(decode_cache_snapshot(realm, noncanonical_owner.bytes(), owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    auto oversized_referrer = encoded;
    auto referrer_marker = "https://owner.example:8443/path?query=one#fragment"sv.bytes();
    bool found_referrer = false;
    for (size_t i = 4; i + referrer_marker.size() <= encoded.size(); ++i) {
        if (encoded.bytes().slice(i, referrer_marker.size()) == referrer_marker) {
            for (size_t j = i - 4; j < i; ++j)
                oversized_referrer[j] = 0xff;
            found_referrer = true;
            break;
        }
    }
    EXPECT(found_referrer);
    EXPECT(decode_cache_snapshot(realm, oversized_referrer.bytes(), owner, CacheStorageIPC::maximum_snapshot_size).is_error());

    for (auto referrer : { "https://user:password@owner.example:8443/path"sv, "ftp://owner.example:8443/path"sv, "https://other.example:8443/path"sv }) {
        request->set_referrer(URL::Parser::basic_parse(referrer).release_value());
        EXPECT(encode_cache_snapshot(*list, owner, CacheStorageIPC::maximum_snapshot_size).is_error());
    }
    auto long_referrer = ByteString::formatted("https://owner.example:8443/{}", ByteString::repeated('a', 65536));
    request->set_referrer(URL::Parser::basic_parse(long_referrer).release_value());
    EXPECT(encode_cache_snapshot(*list, owner, CacheStorageIPC::maximum_snapshot_size).is_error());

    for (auto referrer : { Request::Referrer::Client, Request::Referrer::NoReferrer }) {
        request->set_referrer(referrer);
        auto bytes = MUST(encode_cache_snapshot(*list, owner, CacheStorageIPC::maximum_snapshot_size));
        auto result = GC::make_root(MUST(decode_cache_snapshot(realm, bytes.bytes(), owner, CacheStorageIPC::maximum_snapshot_size)));
        EXPECT(result->elements()[0]->request->referrer().has<Request::Referrer>());
        EXPECT(result->elements()[0]->request->referrer().get<Request::Referrer>() == referrer);
    }
}

TEST_CASE(cache_snapshot_referrer_decoder_enforces_exact_size_limit)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = GC::make_root(create_test_document());
    auto& realm = document->relevant_settings_object().realm();
    HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto owner = URL::Parser::basic_parse("https://owner.example:8443/"sv)->origin();
    auto prefix = "https://owner.example:8443/"sv;
    auto referrer = ByteString::formatted("{}{}", prefix, ByteString::repeated('a', 64 * KiB - prefix.length()));
    auto request = make_request();
    request->set_referrer(URL::Parser::basic_parse(referrer).release_value());
    auto internal = make_response();
    internal->header_list()->delete_("Set-Cookie"sv);
    auto response = GC::make_root(ResponseView::create(realm, internal));
    auto list = GC::make_root(GC::Heap::the().allocate<RequestResponseList>());
    list->elements().append(GC::Heap::the().allocate<RequestResponse>(request, *response));

    auto encoded = MUST(encode_cache_snapshot(*list, owner, CacheStorageIPC::maximum_snapshot_size));
    auto result = GC::make_root(MUST(decode_cache_snapshot(realm, encoded.bytes(), owner, CacheStorageIPC::maximum_snapshot_size)));
    EXPECT_EQ(result->elements()[0]->request->referrer().get<URL::URL>().serialize().bytes().size(), 64 * KiB);

    size_t referrer_offset = 0;
    bool found = false;
    for (size_t i = 4; i + referrer.length() <= encoded.size(); ++i) {
        if (encoded.bytes().slice(i, referrer.length()) == referrer.bytes()) {
            referrer_offset = i;
            found = true;
            break;
        }
    }
    EXPECT(found);
    if (!found)
        return;
    auto end = referrer_offset + referrer.length();
    auto oversized = MUST(ByteBuffer::copy(encoded.bytes().slice(0, end)));
    MUST(oversized.try_append('a'));
    MUST(oversized.try_append(encoded.bytes().slice(end)));
    u32 size = 64 * KiB + 1;
    for (size_t i = 0; i < 4; ++i)
        oversized[referrer_offset - 4 + i] = static_cast<u8>(size >> (8 * i));
    auto error = decode_cache_snapshot(realm, oversized.bytes(), owner, CacheStorageIPC::maximum_snapshot_size);
    EXPECT(error.is_error());
    if (error.is_error()) {
        auto detail = error.release_error();
        EXPECT(detail.is_errno());
        EXPECT_EQ(detail.code(), E2BIG);
    }
}
