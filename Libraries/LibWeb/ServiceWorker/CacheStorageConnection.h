/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/ServiceWorker/CacheStorageProtocol.h>

namespace Web::ServiceWorker {

using CacheStorageReplyCallback = GC::Function<void(CacheStorageIPC::Reply)>;

// The Hub tells each process whether its browsing session has a shared Cache Storage backend. Until it does, every
// environment keeps its own in-memory caches.
WEB_API void set_cache_storage_hub_enabled(bool);
bool cache_storage_hub_is_enabled();

bool cache_storage_environment_is_live(GC::Ptr<HTML::EnvironmentSettingsObject>);
bool cache_storage_promise_is_live(WebIDL::Promise const&);
GC::Ptr<PageClient> cache_storage_page_client(GC::Ptr<HTML::EnvironmentSettingsObject>);
Optional<CacheStorageIPC::StorageError> send_cache_storage_request(GC::Ptr<HTML::EnvironmentSettingsObject>, CacheStorageIPC::Request, GC::Ptr<CacheStorageReplyCallback> = {});
void reject_cache_storage_request(WebIDL::Promise&, CacheStorageIPC::StorageError);
void queue_cache_storage_rejection(WebIDL::Promise&, CacheStorageIPC::StorageError);

}
