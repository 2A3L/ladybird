/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <LibWebView/CacheStoragePartition.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>

namespace WebView {

bool cache_storage_origins_are_secure(ReadonlySpan<URL::Origin> origins)
{
    if (origins.is_empty())
        return false;
    for (auto const& origin : origins) {
        if (origin.is_opaque() || origin.is_file_origin() || !origin.scheme().has_value() || origin.scheme()->is_empty() || origin.scheme() == "resource"sv)
            return false;
        if (Web::SecureContexts::is_origin_potentially_trustworthy(origin) != Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy)
            return false;
    }
    return true;
}

bool cache_storage_creation_url_is_secure(Optional<URL::URL> const& url)
{
    return url.has_value() && url->scheme() != "data"sv
        && Web::SecureContexts::is_url_potentially_trustworthy(*url) == Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy;
}

Optional<String> cache_storage_partition_key(CanonicalEnvironmentSettingsObject const& environment, bool has_cross_site_ancestor)
{
    auto const& origin = environment.origin();
    auto top_origin = environment.top_level_origin();
    if (origin.is_opaque() || origin.is_file_origin() || !origin.scheme().has_value() || origin.scheme()->is_empty())
        return {};
    if (!top_origin.has_value() || top_origin->is_opaque() || top_origin->is_file_origin() || !top_origin->scheme().has_value() || top_origin->scheme()->is_empty())
        return {};
    auto site = network_isolation_top_level_site(environment);
    if (!site.has_value())
        return {};

    auto serialized_origin = origin.serialize();
    auto serialized_site = site->to_utf8();
    constexpr size_t maximum_partition_key_bytes = 4096;
    if (serialized_origin.bytes().size() > maximum_partition_key_bytes || serialized_site.bytes().size() > maximum_partition_key_bytes)
        return {};

    // Length prefixes keep field boundaries unambiguous without trusting delimiters in a URL.
    auto key = String::formatted("{}:{}{}:{}:{}", serialized_origin.bytes().size(), serialized_origin, serialized_site.bytes().size(), serialized_site, has_cross_site_ancestor ? 1 : 0);
    if (key.is_error() || key.value().bytes().size() > maximum_partition_key_bytes)
        return {};
    return key.release_value();
}

Optional<String> cache_storage_dedicated_worker_partition_grant(URL::URL const& script_url, CanonicalEnvironmentSettingsObject const& parent, Optional<String> const& parent_grant)
{
    if (!parent_grant.has_value() || !script_url.scheme().is_one_of("http"sv, "https"sv)
        || !script_url.origin().is_same_origin(parent.origin()))
        return {};
    return parent_grant;
}

}
