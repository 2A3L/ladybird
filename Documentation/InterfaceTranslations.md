# Interface Translations

Ladybird's own interface can be shown in another language than English. The translations are in
`Base/res/ladybird/locale/<language>/`, one JSON file per part of the interface. English is the text in the code, so
there is no English file.

## Where text goes

| File | Text of | Read by |
| --- | --- | --- |
| `chrome.json` | Menu titles and the actions shared by the browser controls and its pages | The browser controls and the pages that are translated |
| `actions.json` | Other actions and context menus | The browser controls |
| `widgets.json` | Windows, toolbars, popovers and dialogs | The browser controls |
| `qt.json` | Text translated with Qt's `tr()` | The browser controls |
| `settings.json` | The settings page | The settings page |
| `pages.json` | Reserved for the other `about:` pages, which aren't translated yet | No page yet |

Each file is a scope. The browser controls read only `chrome`, `actions`, `widgets` and `qt`, and a page reads only
`chrome` and its own file. So the settings page's "Search" section can't translate a "Search" button.

```json
{
    "": {
        "Reload": "Recargar",
        "{0} tab": { "one": "{0} pestaña", "other": "{0} pestañas" }
    },
    "settings-section": { "Search": "Búsqueda" }
}
```

The top level is the context, `""` unless the same English has different meanings in files read together. A plural's
entry is its `one` form, and needs an `other` form, which is also used for a form the catalog leaves out. Arguments are
`{0}` to `{9}`, and `{{` and `}}` are literal braces. A missing or malformed entry shows the English text.

## In the browser controls

- Mark text to translate with `WebView::ui_text("Reload"sv)`, `WebView::ui_text_in("context"sv, "Search"sv)` or
  `WebView::ui_plural(count, "{0} tab"sv, "{0} tabs"sv)`, from `LibWebView/Localization.h`. Their text must be string
  literals. Text from the user or from websites is never marked.
- An `Action` or `Menu` created with such text shows it in the interface language and changes with it.
- For a plain Qt object, `bind_ui_text(object, text, apply)` from `UI/Qt/Localization.h` shows the text now and again
  when the language changes. Text that changes, such as a count, is bound to a slot with
  `bind_ui_text(object, "slot"sv, text, apply)`, which replaces what that slot showed before.
- Text translated with `tr()` is looked up in every file the browser controls read, by its disambiguation as the
  context, but only where Qt names the context after a class of Ladybird's: `tr()` in a `Ladybird::` class that has
  `Q_OBJECT`, or `QObject::tr()`. In a class without `Q_OBJECT`, such as `ProcessManagerWindow`,
  `CrashReportReviewWidget`, `ScreenshotSelectionOverlay`, `ScreenshotViewer`, `ScreenshotThumbnail` or
  `WebCompatReportReview`, `tr()` names the Qt class it derives from, such as `QWidget`, and its text is never
  translated, even though the linter accepts an entry for it. Give such a class `Q_OBJECT`, or use `QObject::tr()` or
  `ui_text`, before translating its text. A widget that sets `tr()` text in its constructor shows a new language once
  it's created again.

## In the browser's pages

Only the settings page is translated so far. A page imports `resource://ladybird/about-pages/l10n.js`. `localizeStaticText(elements)` translates the text and the
`aria-label`, `placeholder`, `title` and `data-title` attributes in the page's markup, except in `translate="no"`
elements, and `data-l10n-context` sets a context. Text added to the page later, or taken out and put back, is
translated by calling `localizeStaticText()` again once it's in the page. Text a script writes is translated with `t()` or `tp()`, and written
again from `onInterfaceLanguageChange()`.

## Checking

`Meta/Linters/lint_localization.py`, which `Meta/lint-ci.sh` runs, checks that every file is valid JSON without
duplicate keys, that each translation has the placeholders of its English text and plurals have an `other` form, that no
text is in two files read together, and that each text is still used where its file is read.

The text of `settings.json` is looked for in `Base/res/ladybird/about-pages` and also in `Libraries/LibWebView`: the
settings page translates the titles and descriptions of the advanced variables and the messages that `Settings.cpp` and
`WebUI/SettingsUI.cpp` give it, as the English text lives in that code.
