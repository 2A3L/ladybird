/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <UI/Qt/Actions.h>

namespace Ladybird {

// NB: These outlive the static destructors, as an action or a menu may be released only after them.
static HashTable<Action*>& all_actions()
{
    static NeverDestroyed<HashTable<Action*>> actions;
    return *actions;
}

static HashTable<Menu*>& all_menus()
{
    static NeverDestroyed<HashTable<Menu*>> menus;
    return *menus;
}

static String resolve_text(ActionText const& text)
{
    return text.visit(
        [](StringView text) { return MUST(String::from_utf8(text)); },
        [](String const& text) { return text; },
        [](WebView::UIText const& text) { return WebView::translate(text); });
}

Action::Action(ActionText text, ActionID id, Function<void()> action)
    : m_text(move(text))
    , m_resolved_text(resolve_text(m_text))
    , m_id(id)
    , m_action(move(action))
{
    all_actions().set(this);
}

Action::~Action()
{
    all_actions().remove(this);
}

void Action::retranslate_all()
{
    Vector<NonnullRefPtr<Action>> actions;
    for (auto* action : all_actions())
        actions.append(*action);
    for (auto& action : actions)
        action->retranslate();

    Vector<NonnullRefPtr<Menu>> menus;
    for (auto* menu : all_menus())
        menus.append(*menu);
    for (auto& menu : menus)
        menu->retranslate({});
}

void Action::retranslate()
{
    if (auto text = resolve_text(m_text); text != m_resolved_text) {
        m_resolved_text = move(text);
        for (auto& observer : m_observers)
            observer->on_text_changed(*this);
    }

    if (!m_tooltip.has_value())
        return;
    if (auto tooltip = resolve_text(*m_tooltip); tooltip != m_resolved_tooltip) {
        m_resolved_tooltip = move(tooltip);
        for (auto& observer : m_observers)
            observer->on_tooltip_changed(*this);
    }
}

NonnullRefPtr<Action> Action::create(ActionText text, ActionID id, Function<void()> action)
{
    return adopt_ref(*new Action { move(text), id, move(action) });
}

NonnullRefPtr<Action> Action::create_checkable(ActionText text, ActionID id, Function<void()> action)
{
    auto checkable = create(move(text), id, move(action));
    checkable->m_checked = false;
    return checkable;
}

void Action::set_text(ActionText text)
{
    // NB: The text is kept even when it reads the same now, as it may not in another language.
    auto resolved_text = resolve_text(text);
    m_text = move(text);
    if (resolved_text == m_resolved_text)
        return;
    m_resolved_text = move(resolved_text);

    for (auto& observer : m_observers)
        observer->on_text_changed(*this);
}

void Action::set_tooltip(ActionText tooltip)
{
    auto resolved_tooltip = resolve_text(tooltip);
    auto had_tooltip = m_tooltip.has_value();
    m_tooltip = move(tooltip);
    if (had_tooltip && resolved_tooltip == m_resolved_tooltip)
        return;
    m_resolved_tooltip = move(resolved_tooltip);

    for (auto& observer : m_observers)
        observer->on_tooltip_changed(*this);
}

void Action::set_enabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;

    for (auto& observer : m_observers)
        observer->on_enabled_state_changed(*this);
}

void Action::set_visible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;

    for (auto& observer : m_observers)
        observer->on_visible_state_changed(*this);
}

void Action::set_engaged(bool engaged)
{
    if (m_engaged == engaged)
        return;
    m_engaged = engaged;

    for (auto& observer : m_observers)
        observer->on_engaged_state_changed(*this);
}

void Action::set_checked(bool checked)
{
    set_checked_internal(checked);

    if (auto group = m_group.strong_ref()) {
        group->for_each_action([&](Action& action) {
            if (action.is_checkable() && &action != this)
                action.set_checked_internal(false);
        });
    }
}

void Action::set_checked_internal(bool checked)
{
    VERIFY(is_checkable());

    if (m_checked == checked)
        return;
    m_checked = checked;

    for (auto& observer : m_observers)
        observer->on_checked_state_changed(*this);
}

void Action::set_busy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;

    for (auto& observer : m_observers)
        observer->on_busy_state_changed(*this);
}

void Action::set_shortcut_hint(Optional<ActionID> shortcut_hint)
{
    if (m_shortcut_hint == shortcut_hint)
        return;
    m_shortcut_hint = shortcut_hint;

    for (auto& observer : m_observers)
        observer->on_shortcut_hint_changed(*this);
}

void Action::add_observer(NonnullOwnPtr<Observer> observer)
{
    observer->on_text_changed(*this);
    if (m_tooltip.has_value())
        observer->on_tooltip_changed(*this);
    observer->on_enabled_state_changed(*this);
    observer->on_visible_state_changed(*this);
    observer->on_engaged_state_changed(*this);
    observer->on_busy_state_changed(*this);
    if (is_checkable())
        observer->on_checked_state_changed(*this);
    observer->on_shortcut_hint_changed(*this);

    m_observers.append(move(observer));
}

void Action::remove_observer(Observer const& observer)
{
    m_observers.remove_first_matching([&](auto const& candidate) {
        return candidate.ptr() == &observer;
    });
}

Menu::Menu(ActionText title)
    : m_title(move(title))
    , m_resolved_title(resolve_text(m_title))
{
    all_menus().set(this);
}

Menu::~Menu()
{
    all_menus().remove(this);
}

void Menu::retranslate(Badge<Action>)
{
    auto title = resolve_text(m_title);
    if (title == m_resolved_title)
        return;
    m_resolved_title = move(title);

    for (auto& observer : m_observers)
        observer->on_title_changed(*this);
}

NonnullRefPtr<Menu> Menu::create(ActionText title)
{
    return adopt_ref(*new Menu { move(title) });
}

NonnullRefPtr<Menu> Menu::create_group(ActionText title)
{
    auto menu = create(move(title));
    menu->m_is_group = true;
    return menu;
}

void Menu::add_action(NonnullRefPtr<Action> action)
{
    if (m_is_group)
        action->set_group({}, *this);
    m_items.append(move(action));
}

void Menu::set_visible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;

    for (auto& observer : m_observers)
        observer->on_visible_state_changed(*this);
}

void Menu::add_observer(NonnullOwnPtr<Observer> observer)
{
    observer->on_visible_state_changed(*this);

    m_observers.append(move(observer));
}

void Menu::remove_observer(Observer const& observer)
{
    m_observers.remove_first_matching([&](auto const& candidate) {
        return candidate.ptr() == &observer;
    });
}

}
