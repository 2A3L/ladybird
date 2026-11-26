/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Loader/LoaderConfig.h>
#include <LibWeb/ServiceWorker/CacheResponsePolicy.h>

namespace {

class TransferTrackingResponse final : public Web::Fetch::Infrastructure::Response {
public:
    static NonnullRefPtr<TransferTrackingResponse> create()
    {
        return adopt_ref(*new TransferTrackingResponse);
    }

    bool transfer_lease_was_released() const { return m_transfer_lease_was_released; }
    virtual void release_request_transfer_lease() const override { m_transfer_lease_was_released = true; }

private:
    TransferTrackingResponse()
        : Response(HTTP::HeaderList::create())
    {
    }

    mutable bool m_transfer_lease_was_released { false };
};

}

TEST_CASE(http_redirect_fetch_releases_intermediate_response_transfer_lease)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm();

    auto request_url = URL::Parser::basic_parse("https://example.test/start"sv);
    VERIFY(request_url.has_value());

    auto request = Web::Fetch::Infrastructure::Request::create();
    request->set_url_list({ request_url.release_value() });
    request->set_redirect_count(20);

    auto algorithms = Web::Fetch::Infrastructure::FetchAlgorithms::create(Web::Fetch::Infrastructure::FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent });
    Web::Fetch::Infrastructure::FetchControllerRegistry controller_registry;
    auto controller = GC::make_root(Web::Fetch::Infrastructure::FetchController::create(controller_registry));
    auto fetch_params = Web::Fetch::Infrastructure::FetchParams::create(*controller, request, algorithms, Web::Fetch::Infrastructure::FetchTimingInfo::create(), Web::current_loader_config());
    auto response = TransferTrackingResponse::create();
    response->set_status(302);
    response->set_header_list(HTTP::HeaderList::create({ { "Location"sv, "/redirected"sv } }));

    auto pending_response = Web::Fetch::Fetching::http_redirect_fetch(realm, fetch_params, response);

    VERIFY(pending_response);
    EXPECT(response->transfer_lease_was_released());
}

TEST_CASE(cached_opaque_response_obeys_embedder_policy)
{
    using Web::Fetch::Infrastructure::OpaqueFilteredResponse;
    using Web::Fetch::Infrastructure::Response;
    using Web::HTML::EmbedderPolicy;
    using Web::HTML::EmbedderPolicyValue;
    using Web::ServiceWorker::cached_response_is_blocked_by_cross_origin_policy;

    auto resource_url = URL::Parser::basic_parse("https://resource.test/fixture"sv);
    auto document_url = URL::Parser::basic_parse("https://document.test/"sv);
    VERIFY(resource_url.has_value());
    VERIFY(document_url.has_value());

    auto resource_origin = resource_url->origin();
    auto document_origin = document_url->origin();
    EmbedderPolicy requires_corp;
    requires_corp.value = EmbedderPolicyValue::RequireCorp;

    auto response = Response::create();
    response->set_url_list({ resource_url.release_value() });
    auto opaque_response = OpaqueFilteredResponse::create(response);

    EXPECT(!cached_response_is_blocked_by_cross_origin_policy(document_origin, requires_corp, *response));
    EXPECT(!cached_response_is_blocked_by_cross_origin_policy(document_origin, {}, *opaque_response));
    EXPECT(!cached_response_is_blocked_by_cross_origin_policy(resource_origin, requires_corp, *opaque_response));
    EXPECT(cached_response_is_blocked_by_cross_origin_policy(document_origin, requires_corp, *opaque_response));

    response->set_header_list(HTTP::HeaderList::create({ { "Cross-Origin-Resource-Policy"sv, "cross-origin"sv } }));
    EXPECT(!cached_response_is_blocked_by_cross_origin_policy(document_origin, requires_corp, *opaque_response));

    auto opaque_response_without_url = OpaqueFilteredResponse::create(Response::create());
    EXPECT(cached_response_is_blocked_by_cross_origin_policy(document_origin, requires_corp, *opaque_response_without_url));
}
