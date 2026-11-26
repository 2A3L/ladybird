/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CacheStorageStore.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

static u64 s_next_id { 1 };

BrowsingSession::BrowsingSession(IsPrivate is_private)
    : m_id(s_next_id++)
    , m_is_private(is_private)
{
}

BrowsingSession::~BrowsingSession() = default;

bool BrowsingSession::install_cache_storage_store(NonnullOwnPtr<CacheStorageStore> store)
{
    if (m_cache_storage_store)
        return false;
    m_cache_storage_store = move(store);

    // Processes of this session that are already running start using the store too.
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (&client.session() == this)
            client.async_set_cache_storage_hub_enabled(true);
        return IterationDecision::Continue;
    });
    return true;
}

NonnullRefPtr<BrowsingSession> BrowsingSession::create(IsPrivate is_private)
{
    auto session = adopt_ref(*new BrowsingSession(is_private));

    if (is_private == IsPrivate::Yes) {
        session->cookie_jar = CookieJar::create(IsPrivate::Yes);
        session->storage_jar = StorageJar::create();
        session->blob_url_store = make<BlobURLStore>();
        session->hsts_store = HSTSStore::create();
    }

    return session;
}

}
