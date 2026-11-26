#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import runpy
import subprocess
import tempfile

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
    };
"""


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

            english = script(READ_PAGE)
            assert english["title"] == "Settings", english
            assert english["appearance"] == "Appearance", english

            set_language_and_wait_for_title("es", "Configuración")
            spanish = script(READ_PAGE)
            assert spanish["title"] == "Configuración", spanish
            assert spanish["appearance"] == "Apariencia", spanish
            assert spanish["search"] == "Buscar en la configuración", spanish
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
