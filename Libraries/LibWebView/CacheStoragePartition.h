/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Empty chains, opaque origins, file and internal resource origins are denied even where
// secure-context trustworthiness treats file and resource origins as potentially trustworthy.
WEBVIEW_API bool cache_storage_origins_are_secure(ReadonlySpan<URL::Origin>);
WEBVIEW_API bool cache_storage_creation_url_is_secure(Optional<URL::URL> const&);

// This fork partitions the shared Cache Storage backend by origin, top-level site and a single cross-site-ancestor bit
// (whether any ancestor is cross-site with the top-level site), not by the full ancestor chain: A->B->A and A->C->A
// share a partition. The current Storage Standard's storage key and Ladybird's ordinary StorageKey contain only the origin.
// The caller is the source of truth for that bit. A worker environment carries its own has_cross_site_ancestor();
// callers pass that value, and it is never recomputed here.
WEBVIEW_API Optional<String> cache_storage_partition_key(CanonicalEnvironmentSettingsObject const&, bool has_cross_site_ancestor);
// A dedicated worker inherits a grant only for a same-origin HTTP(S) script.
WEBVIEW_API Optional<String> cache_storage_dedicated_worker_partition_grant(URL::URL const&, CanonicalEnvironmentSettingsObject const&, Optional<String> const& parent_grant);

}
