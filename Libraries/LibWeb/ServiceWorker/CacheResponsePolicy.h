/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>

namespace Web::ServiceWorker {

[[nodiscard]] WEB_API bool cached_response_is_blocked_by_cross_origin_policy(URL::Origin const&, HTML::EmbedderPolicy const&, Fetch::Infrastructure::Response const&);

}
