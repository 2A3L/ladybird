/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/CacheStoragePartition.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>

static URL::Origin origin_for(StringView url)
{
    return URL::Parser::basic_parse(url).value().origin();
}

static Optional<String> key_for(URL::Origin origin, Optional<URL::Origin> top_origin, bool has_cross_site_ancestor)
{
    WebView::CanonicalWorkerEnvironmentSettingsObject environment { move(origin), move(top_origin), has_cross_site_ancestor, Web::HTML::EnvironmentId::generate() };
    return WebView::cache_storage_partition_key(environment, has_cross_site_ancestor);
}

TEST_CASE(cache_storage_partition_includes_origin_top_level_site_and_ancestor)
{
    auto origin = origin_for("https://a.example:8443/"sv);
    auto top = origin_for("https://site.example/"sv);
    auto key = key_for(origin, top, false);
    EXPECT(key.has_value());
    EXPECT(key == key_for(origin, top, false));
    EXPECT(key != key_for(origin_for("https://a.example:9443/"sv), top, false));
    EXPECT(key != key_for(origin, origin_for("https://other.test/"sv), false));
    EXPECT(key != key_for(origin, origin_for("http://site.example/"sv), false));
    EXPECT(key != key_for(origin, top, true));
}

TEST_CASE(cache_storage_partition_rejects_missing_opaque_and_file_origins)
{
    auto origin = origin_for("https://a.example/"sv);
    auto opaque = URL::Origin::create_opaque();
    auto file = origin_for("file:///tmp/test.html"sv);
    EXPECT(!key_for(origin, {}, false).has_value());
    EXPECT(!key_for(opaque, origin, false).has_value());
    EXPECT(!key_for(origin, opaque, false).has_value());
    EXPECT(!key_for(file, origin, false).has_value());
    EXPECT(!key_for(origin, file, false).has_value());
}

TEST_CASE(cache_storage_requires_a_secure_nonempty_origin_chain)
{
    EXPECT(!WebView::cache_storage_creation_url_is_secure({}));
    EXPECT(!WebView::cache_storage_creation_url_is_secure(URL::Parser::basic_parse("data:text/html,test"sv)));
    EXPECT(!WebView::cache_storage_creation_url_is_secure(URL::Parser::basic_parse("http://insecure.example/"sv)));
    EXPECT(WebView::cache_storage_creation_url_is_secure(URL::Parser::basic_parse("http://localhost/"sv)));
    EXPECT(WebView::cache_storage_creation_url_is_secure(URL::Parser::basic_parse("https://example.test/"sv)));
    Vector<URL::Origin> chain;
    EXPECT(!WebView::cache_storage_origins_are_secure(chain.span()));
    chain.append(origin_for("https://child.example/"sv));
    chain.append(origin_for("https://parent.example/"sv));
    EXPECT(WebView::cache_storage_origins_are_secure(chain.span()));
    chain.append(origin_for("http://insecure.example/"sv));
    EXPECT(!WebView::cache_storage_origins_are_secure(chain.span()));
    chain.take_last();
    chain.append(origin_for("http://localhost:8080/"sv));
    EXPECT(WebView::cache_storage_origins_are_secure(chain.span()));
    chain.take_last();
    chain.append(origin_for("http://127.0.0.1/"sv));
    EXPECT(WebView::cache_storage_origins_are_secure(chain.span()));
    chain.take_last();
    chain.append(URL::Origin::create_opaque());
    EXPECT(!WebView::cache_storage_origins_are_secure(chain.span()));
    chain.take_last();
    chain.append(origin_for("file:///tmp/test.html"sv));
    EXPECT(!WebView::cache_storage_origins_are_secure(chain.span()));
    chain.take_last();
    chain.append(origin_for("resource:///test.html"sv));
    EXPECT(!WebView::cache_storage_origins_are_secure(chain.span()));
}

TEST_CASE(dedicated_workers_inherit_only_same_origin_http_grants)
{
    auto origin = origin_for("https://worker.example/"sv);
    WebView::CanonicalWorkerEnvironmentSettingsObject parent { origin, origin, false, Web::HTML::EnvironmentId::generate() };
    auto grant = WebView::cache_storage_partition_key(parent, false);
    EXPECT(grant.has_value());
    auto same_origin = URL::Parser::basic_parse("https://worker.example/worker.js"sv).value();
    auto cross_origin = URL::Parser::basic_parse("https://other.example/worker.js"sv).value();
    auto wrong_scheme = URL::Parser::basic_parse("http://worker.example/worker.js"sv).value();
    auto blob = URL::Parser::basic_parse("blob:https://worker.example/123"sv).value();
    auto inherited = WebView::cache_storage_dedicated_worker_partition_grant(same_origin, parent, grant);
    EXPECT(inherited == grant);
    EXPECT(WebView::cache_storage_dedicated_worker_partition_grant(same_origin, parent, inherited) == grant);
    EXPECT(!WebView::cache_storage_dedicated_worker_partition_grant(same_origin, parent, {}).has_value());
    EXPECT(!WebView::cache_storage_dedicated_worker_partition_grant(cross_origin, parent, grant).has_value());
    EXPECT(!WebView::cache_storage_dedicated_worker_partition_grant(wrong_scheme, parent, grant).has_value());
    EXPECT(!WebView::cache_storage_dedicated_worker_partition_grant(blob, parent, grant).has_value());

    auto local_origin = origin_for("http://127.0.0.1:8080/"sv);
    WebView::CanonicalWorkerEnvironmentSettingsObject local_parent { local_origin, local_origin, false, Web::HTML::EnvironmentId::generate() };
    auto local_grant = WebView::cache_storage_partition_key(local_parent, false);
    auto local_script = URL::Parser::basic_parse("http://127.0.0.1:8080/worker.js"sv).value();
    EXPECT(local_grant.has_value());
    EXPECT(WebView::cache_storage_dedicated_worker_partition_grant(local_script, local_parent, local_grant) == local_grant);
}
