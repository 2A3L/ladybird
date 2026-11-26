/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <UI/Qt/Actions.h>
#include <UI/Qt/Localization.h>
#include <UI/Qt/StringUtils.h>

#include <QObject>

namespace Ladybird {

class UITextBinding;

static HashTable<UITextBinding*>& all_bindings()
{
    static NeverDestroyed<HashTable<UITextBinding*>> bindings;
    return *bindings;
}

// A child of the object that shows the text, so it goes away along with that object.
class UITextBinding final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    UITextBinding(QObject& owner, StringView slot, WebView::UIText text, Function<void(QString const&)> apply)
        : QObject(&owner)
        , m_slot(MUST(String::from_utf8(slot)))
        , m_text(move(text))
        , m_apply(move(apply))
    {
        all_bindings().set(this);
        apply_text();
    }

    virtual ~UITextBinding() override
    {
        all_bindings().remove(this);
    }

    StringView slot() const { return m_slot; }

    void rebind(WebView::UIText text, Function<void(QString const&)> apply)
    {
        m_text = move(text);
        m_apply = move(apply);
        apply_text();
    }

    void apply_text() { m_apply(translate_to_qstring(m_text)); }

private:
    String m_slot;
    WebView::UIText m_text;
    Function<void(QString const&)> m_apply;
};

QString translate_to_qstring(WebView::UIText const& text)
{
    return qstring_from_ak_string(WebView::translate(text));
}

void bind_ui_text(QObject& owner, WebView::UIText text, Function<void(QString const&)> apply)
{
    new UITextBinding(owner, {}, move(text), move(apply));
}

void bind_ui_text(QObject& owner, StringView slot, WebView::UIText text, Function<void(QString const&)> apply)
{
    VERIFY(!slot.is_empty());
    for (auto* binding : all_bindings()) {
        if (binding->parent() == &owner && binding->slot() == slot) {
            binding->rebind(move(text), move(apply));
            return;
        }
    }
    new UITextBinding(owner, slot, move(text), move(apply));
}

void apply_interface_language(StringView language)
{
    if (!WebView::set_interface_language(language))
        return;

    Action::retranslate_all();

    Vector<UITextBinding*> bindings;
    for (auto* binding : all_bindings())
        bindings.append(binding);
    for (auto* binding : bindings)
        binding->apply_text();
}

}
