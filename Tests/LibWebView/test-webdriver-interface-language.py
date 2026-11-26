#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import runpy
import subprocess
import tempfile
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

READ_PAGE = """
    return {
        title: document.title,
        appearance: document.querySelector('#tab-button-appearance').textContent.trim(),
        search: document.querySelector('#settings-search').placeholder,
        hint: document.querySelector('label[for=interface-language] .description').textContent.trim(),
        hintLanguage: document.querySelector('label[for=interface-language] .description').getAttribute('lang'),
        english: document.querySelector('#interface-language option[value=en]').textContent,
        selected: document.querySelector('#interface-language').value,
        swatch: document.querySelector('.color-swatch[title]').getAttribute('title'),
        // A dialog's list and title are made when it is opened, and show the language it was opened in.
        texts: [...document.querySelectorAll(TEXT_SELECTOR)]
            .filter(element => !element.closest('.dialog-list') && !element.id.endsWith('-title'))
            .map(ownText),
    };
"""

# The text a script doesn't make by itself, and so is there from the start, without the sizes and statuses that arrive
# later. A heading or a label's own words, without the description inside it.
TEXT_HELPERS = """
    const ownText = element =>
        [...element.childNodes].filter(node => node.nodeType === Node.TEXT_NODE).map(node => node.data).join('')
            .replace(/\\s+/g, ' ').trim();
    const TEXT_SELECTOR = [
        '.tab-bar button', '.tab-panel h3', '.tab-panel label', '.tab-panel button', '.tab-panel option',
        '.tab-panel .description:not([id])', '.tab-panel .list-heading span', '.config-row p',
        'dialog h3', 'dialog label', 'dialog button', 'dialog p',
    ].join(',');
"""

READ_PAGE = TEXT_HELPERS + READ_PAGE

# The heading and a control of each tab, in Spanish.
TAB_TEXTS = {
    "appearance": [("#tab-layout-title", "Disposición"), ("label[for=enable-force-dark]", "Forzar modo oscuro")],
    "search": [("#search-engine-title", "Motor de búsqueda"), ("#search-settings", "Motores personalizados…")],
    "browsing": [
        ("#tab-browsing .card-title", "Inicio"),
        ("label[for=restore-previous-session]", "Restaurar las ventanas y pestañas anteriores"),
    ],
    "memory": [
        ("#tab-memory .card-title", "Pestañas"),
        ("label[for=tab-processes]", "Permitir que las pestañas compartan un proceso"),
    ],
    "privacy": [("#tab-privacy .card-title", "Datos de navegación"), ("#browsing-data-settings", "Borrar…")],
    "permissions": [
        ("label[for=autoplay-policy]", "Reproducción automática"),
        ("#geolocation-settings", "Excepciones…"),
    ],
    "blocking": [
        ("label[for=enable-content-blocking]", "Bloquear anuncios, rastreadores y más"),
        ("#add-filter-list", "Añadir lista…"),
    ],
    "feedback": [("#crash-reports-title", "Informes de fallos"), ("#open-crash-reports", "Abrir carpeta")],
    "advanced": [
        ("label[for=dns-upstream]", "Proveedor de DNS"),
        ("#tab-advanced .card-title:last-of-type", "Desarrollo"),
    ],
}

OWN_TEXT = TEXT_HELPERS + "return ownText(document.querySelector(arguments[0]));"

# A button that never wraps can't overflow itself, so a control also has to stay inside the card or dialog it is in,
# and the page itself must not scroll sideways.
FIND_OVERFLOWS = """
    const overflows = [];
    const describe = element =>
        `${element.tagName} ${element.id || element.className} ${element.textContent.trim().slice(0, 40)}`;
    for (const element of document.querySelectorAll('.tab-button, label, button')) {
        if (element.closest('.visually-hidden')) continue;
        if (element.getClientRects().length === 0 || element.clientWidth === 0) continue;
        if (element.scrollWidth > element.clientWidth) overflows.push(describe(element));
        // The tabs of a narrow window scroll in their bar by design.
        if (element.matches('.tab-button')) continue;
        const container = element.closest('dialog, .card');
        if (container && element.getBoundingClientRect().right > container.getBoundingClientRect().right + 1)
            overflows.push(`outside its container: ${describe(element)}`);
    }
    const scrolling = document.scrollingElement;
    if (scrolling.scrollWidth > window.innerWidth) overflows.push(`page: ${scrolling.scrollWidth}`);
    return {width: window.innerWidth, overflows};
"""

OPEN_DIALOG = "location.hash = arguments[0];"

SHOW_TAB = "document.querySelector('#tab-button-' + arguments[0]).click();"


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory() as directory:
        process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(path, body):
                status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def script(source):
                return command("execute/sync", {"script": source, "args": []})

            def set_language_and_wait_for_title(language, title):
                command(
                    "execute/async",
                    {
                        "script": """
                        const [language, title, done] = arguments;
                        ladybird.sendMessage('setConfigVariable', {name: 'ui.interface.language', value: language});
                        const deadline = Date.now() + 10000;
                        (function check() {
                            if (document.title === title || Date.now() > deadline) return done();
                            setTimeout(check, 20);
                        })();
                    """,
                        "args": [language, title],
                    },
                )

            def wait_for(source, timeout=10):
                deadline = time.monotonic() + timeout
                while not script(f"return Boolean({source});"):
                    assert time.monotonic() < deadline, f"Timed out waiting for {source}"
                    time.sleep(0.05)

            command("url", {"url": "about:settings"})
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    if (document.readyState === 'complete') return done();
                    window.addEventListener('load', () => done());
                """,
                    "args": [],
                },
            )

            # The page asks for the browser's settings once it has loaded.
            wait_for("document.querySelector('.config-row')")
            command(
                "execute/sync",
                {
                    "script": """
                    ladybird.sendMessage('addCustomSearchEngine',
                        {name: 'Search', url: 'https://example.com/?q=%s', suggestionsUrl: ''});
                """,
                    "args": [],
                },
            )
            wait_for(
                "[...document.querySelectorAll('#search-engine .engine-name')].some(n => n.textContent === 'Search')"
            )

            english = script(READ_PAGE)
            assert english["title"] == "Settings", english
            assert english["appearance"] == "Appearance", english

            set_language_and_wait_for_title("es", "Configuración")
            spanish = script(READ_PAGE)
            assert spanish["title"] == "Configuración", spanish
            assert spanish["appearance"] == "Apariencia", spanish
            assert spanish["search"] == "Buscar ajustes", spanish
            assert spanish["hint"] == "Algunas partes solo están disponibles en inglés.", spanish
            assert spanish["hintLanguage"] == "es", spanish
            assert spanish["english"] == "English", "A language's own name must not be translated"
            assert spanish["selected"] == "es", spanish
            assert spanish["swatch"] == "Sistema", spanish

            # Recording the same text again must not take its translation for its English.
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    import('resource://ladybird/about-pages/l10n.js').then(l10n => {
                        l10n.localizeStaticText([document.querySelector('.settings-sidebar')]);

                        // Text recorded before it's in the page is translated once it's recorded again there.
                        const added = document.createElement('p');
                        added.id = 'added-text';
                        added.textContent = 'Appearance';
                        l10n.localizeStaticText([added]);
                        document.querySelector('#tab-appearance').append(added);
                        l10n.localizeStaticText([added]);
                        done();
                    });
                """,
                    "args": [],
                },
            )

            assert script("return document.querySelector('#added-text').textContent;") == "Apariencia"
            for tab, texts in TAB_TEXTS.items():
                command("execute/sync", {"script": SHOW_TAB, "args": [tab]})
                for selector, text in texts:
                    found = command("execute/sync", {"script": OWN_TEXT, "args": [selector]})
                    assert found == text, (tab, selector, found, text)
            advanced_title = command(
                "execute/sync",
                {
                    "script": "return document.getElementById('security.certificates.offer_to_trust_self_signed')"
                    ".closest('.config-row').querySelector('p').textContent;",
                    "args": [],
                },
            )
            assert advanced_title == "Ofrecer confiar en certificados autofirmados", advanced_title

            # A name the user chose is not the browser's text, even when it is a word the browser translates.
            command("execute/sync", {"script": SHOW_TAB, "args": ["search"]})
            engines = script(
                "return [...document.querySelectorAll('#search-engine .engine-name')].map(n => n.textContent);"
            )
            assert "Search" in engines and "Ninguno" in engines, engines

            for width in (1280, 800, 600):
                status, _, raw = helpers["request"](
                    port, "POST", f"/session/{session}/window/rect", {"width": width, "height": 900}
                )
                assert status == 200, raw
                for tab in TAB_TEXTS:
                    command("execute/sync", {"script": SHOW_TAB, "args": [tab]})
                    found = script(FIND_OVERFLOWS)
                    # A window that kept its size would pass the narrow widths without checking them.
                    assert width - 80 <= found["width"] <= width, (width, found["width"])
                    assert found["overflows"] == [], (width, tab, found)

                # The dialogs have controls of their own.
                for tab, dialog, dialog_id in (
                    ("search", "searchEngines", "search-dialog"),
                    ("browsing", "editLanguages", "languages-dialog"),
                    ("permissions", "autoplay", "site-settings"),
                    ("privacy", "clearBrowsingData", "browsing-data-settings-dialog"),
                ):
                    command("execute/sync", {"script": SHOW_TAB, "args": [tab]})
                    command("execute/sync", {"script": OPEN_DIALOG, "args": [dialog]})
                    wait_for(f"document.querySelector('#{dialog_id}[open]')")
                    found = script(FIND_OVERFLOWS)
                    assert found["overflows"] == [], (width, dialog, found)
                    script("document.querySelector('dialog[open]').close(); return true;")
                    wait_for("!document.querySelector('dialog[open]')")

            # The search finds a setting by its Spanish text without its accents, and by its English text.
            command("execute/sync", {"script": SHOW_TAB, "args": ["privacy"]})
            for query in ("busqueda de actualizaciones", "update checks"):
                found = script(
                    f"""
                    const input = document.querySelector('#settings-search');
                    input.value = {query!r};
                    input.dispatchEvent(new Event('input', {{bubbles: true}}));
                    const row = document.querySelector('#update-checks').closest('.card-group');
                    const other = document.querySelector('#filter-list-updates').closest('.card-group');
                    return {{
                        found: !row.classList.contains('search-miss'),
                        other: other.classList.contains('search-miss'),
                        empty: document.querySelector('#search-empty').hidden,
                    }};
                """
                )
                assert found == {"found": True, "other": True, "empty": True}, (query, found)
            script(
                "document.querySelector('#settings-search').value = ''; document.querySelector('#settings-search').dispatchEvent(new Event('input', {bubbles: true}));"
            )

            set_language_and_wait_for_title("en", "Settings")
            assert script("return document.querySelector('#added-text').textContent;") == "Appearance"
            script("document.querySelector('#added-text').remove();")
            restored = script(READ_PAGE)
            assert restored == english, (restored, english)
            print("PASS: the settings page changes its interface language and back without reloading")
        finally:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
