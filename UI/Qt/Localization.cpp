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

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTranslator>

#include <string.h>

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

// Text that Qt translates, such as with tr(), comes from the catalog too. Its context is the catalog's, and Qt's own
// context, the class name, only tells Ladybird's text from Qt's: Qt's own widgets have text, such as "Select All" in a
// text field's menu, that must not be translated by entries meant for Ladybird's.
class CatalogTranslator final : public QTranslator {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit CatalogTranslator(QObject* parent)
        : QTranslator(parent)
    {
    }

    virtual QString translate(char const* qt_context, char const* source_text, char const* disambiguation, int n) const override
    {
        // NB: The catalog belongs to the GUI thread.
        if (!qt_context || !source_text || QThread::currentThread() != thread())
            return {};
        if (!is_ladybird_context({ qt_context, strlen(qt_context) }))
            return {};

        auto context = disambiguation ? StringView { disambiguation, strlen(disambiguation) } : ""sv;
        auto source = StringView { source_text, strlen(source_text) };
        auto translation = WebView::find_translation(context, source, n >= 0 ? Optional<i64> { n } : Optional<i64> {});
        if (!translation.has_value())
            return {};
        return qstring_from_ak_string(*translation);
    }

    // A class of Ladybird's, or QObject, for QObject::tr() in Ladybird's own functions.
    static bool is_ladybird_context(StringView qt_context)
    {
        return qt_context.starts_with("Ladybird::"sv) || qt_context == "QObject"sv;
    }

    // NB: Qt only announces the language change of a translator that isn't empty.
    virtual bool isEmpty() const override { return false; }
};

// Widgets that set their text in changeEvent() learn of a new language from Qt's LanguageChange event.
static void announce_language_change()
{
    auto* application = QCoreApplication::instance();
    if (!application)
        return;

    static QPointer<CatalogTranslator> translator;
    if (!translator) {
        translator = new CatalogTranslator(application);
        QCoreApplication::installTranslator(translator);
        return;
    }

    QEvent event(QEvent::LanguageChange);
    QCoreApplication::sendEvent(application, &event);
}

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

    announce_language_change();
}

}
