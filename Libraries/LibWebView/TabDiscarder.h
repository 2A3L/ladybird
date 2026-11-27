/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

// Closes the spare WebContent process and discards hidden tabs when the system is about to run out of memory.
// Tabs are also discarded once they were hidden for as long as the user's settings allow.
class WEBVIEW_API TabDiscarder {
    AK_MAKE_NONCOPYABLE(TabDiscarder);
    AK_MAKE_NONMOVABLE(TabDiscarder);

public:
    AK_ALLOC_WITH_KMALLOC;

    TabDiscarder();
    ~TabDiscarder();

    // Check while there is a hidden tab or a spare process whose memory can be reclaimed.
    void schedule_check();
    static void did_hide_a_tab();

    // How long a tab must stay hidden before it is discarded, if hidden tabs are discarded at all.
    static Optional<AK::Duration> minimum_time_hidden_after_inactivity();

private:
    void check_tabs();

    NonnullRefPtr<Core::Timer> m_timer;
};

}
