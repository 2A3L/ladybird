/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibCore/Platform/MemoryPressure.h>
#include <LibCore/Timer.h>
#include <LibWebView/Application.h>
#include <LibWebView/TabDiscarder.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

static constexpr auto check_interval = AK::Duration::from_seconds(5);

// A tab that the user saw a short while ago is likely to be one they come back to soon.
static constexpr auto minimum_time_hidden_under_memory_pressure = AK::Duration::from_seconds(10 * 60);

static TabDiscarder* s_the;

Optional<AK::Duration> TabDiscarder::minimum_time_hidden_after_inactivity()
{
    // NB: Without a discarder, as when the browser runs headless, no tab is discarded.
    if (!s_the)
        return {};
    auto minutes = Application::settings().tab_settings().discard_hidden_tabs_after_minutes;
    if (minutes == 0)
        return {};
    return AK::Duration::from_seconds(static_cast<i64>(minutes) * 60);
}

TabDiscarder::TabDiscarder()
    : m_timer(Core::Timer::create_repeating(check_interval, [this] { check_tabs(); }))
{
    VERIFY(!s_the);
    s_the = this;
}

TabDiscarder::~TabDiscarder()
{
    s_the = nullptr;
}

void TabDiscarder::did_hide_a_tab()
{
    if (s_the)
        s_the->schedule_check();
}

void TabDiscarder::schedule_check()
{
    m_timer->start();
}

void TabDiscarder::check_tabs()
{
    auto has_hidden_tab = false;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        // NB: A tab that the UI was asked to discard isn't asked about again until the user has seen it.
        if (!view.supports_discarding() || !view.hidden_since().has_value() || view.discard_requested())
            return IterationDecision::Continue;
        has_hidden_tab = true;
        return IterationDecision::Break;
    });
    auto& application = Application::the();
    auto has_spare_web_content_process = application.has_spare_web_content_process();
    if (!has_hidden_tab && !has_spare_web_content_process) {
        m_timer->stop();
        return;
    }

    auto memory_pressure_is_critical = Core::Platform::system_memory_pressure() == Core::Platform::MemoryPressure::Critical;
    if (memory_pressure_is_critical && has_spare_web_content_process)
        application.close_spare_web_content_process();

    if (!has_hidden_tab) {
        if (memory_pressure_is_critical)
            m_timer->stop();
        return;
    }

    auto now = MonotonicTime::now();

    // NB: The memory of a discarded tab comes back as its processes exit, so one tab is discarded at a time, and the
    //     next check sees whether the system still needs more. The tab the user saw least recently goes first, but a
    //     tab whose processes all host other tabs too frees little, so one that lets a process exit goes before it.
    // If the spare was closed, give the system until the next check to reclaim its memory before discarding a tab.
    if (memory_pressure_is_critical && !has_spare_web_content_process) {
#ifdef AK_OS_LINUX
        Vector<ViewImplementation*> hidden_views;
        constexpr auto keep_full_frame = DiscardBlockers::NotATab | DiscardBlockers::Visible | DiscardBlockers::Pinned
            | DiscardBlockers::CurrentTabOfWindow | DiscardBlockers::Loading | DiscardBlockers::Crashed
            | DiscardBlockers::PlayingAudio | DiscardBlockers::RecentlyPlayedAudio | DiscardBlockers::CapturingAudio
            | DiscardBlockers::OwnsAView;
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (!view.hidden_since().has_value() || view.discard_requested()
                || has_any_flag(view.discard_blockers(ViewImplementation::DiscardReason::MemoryPressure, now), keep_full_frame))
                return IterationDecision::Continue;
            hidden_views.append(&view);
            return IterationDecision::Continue;
        });
        quick_sort(hidden_views, [](auto* left, auto* right) { return *left->hidden_since() < *right->hidden_since(); });
        Vector<Web::CompositorContextId> hidden_contexts;
        hidden_contexts.ensure_capacity(hidden_views.size());
        for (auto* view : hidden_views)
            hidden_contexts.unchecked_append(view->page().compositor_context_id());
        application.retire_hidden_software_frames(move(hidden_contexts));
#endif

        ViewImplementation* least_recently_seen = nullptr;
        ViewImplementation* least_recently_seen_ending_a_process = nullptr;
        auto seen_less_recently = [](ViewImplementation& view, ViewImplementation* other) {
            return !other || *view.hidden_since() < *other->hidden_since();
        };
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (!view.may_be_discarded(ViewImplementation::DiscardReason::MemoryPressure, minimum_time_hidden_under_memory_pressure, now))
                return IterationDecision::Continue;
            if (seen_less_recently(view, least_recently_seen))
                least_recently_seen = &view;
            if (seen_less_recently(view, least_recently_seen_ending_a_process) && view.discarding_ends_a_process())
                least_recently_seen_ending_a_process = &view;
            return IterationDecision::Continue;
        });
        if (auto* view = least_recently_seen_ending_a_process ? least_recently_seen_ending_a_process : least_recently_seen) {
            dbgln("Discarding tab {}, as the system is about to run out of memory", view->url());
            view->request_discard(ViewImplementation::DiscardReason::MemoryPressure);
        }
    }

    auto minimum_time_hidden = minimum_time_hidden_after_inactivity();
    if (!minimum_time_hidden.has_value())
        return;

    Vector<ViewImplementation&> inactive_views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.may_be_discarded(ViewImplementation::DiscardReason::Inactivity, *minimum_time_hidden, now))
            inactive_views.append(view);
        return IterationDecision::Continue;
    });
    for (auto& view : inactive_views) {
        dbgln("Discarding tab {}, as it was hidden for {} min", view.url(), minimum_time_hidden->to_seconds() / 60);
        view.request_discard(ViewImplementation::DiscardReason::Inactivity);
    }
}

}
