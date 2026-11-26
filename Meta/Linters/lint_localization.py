#!/usr/bin/env python3

# Checks the interface translation catalogs in Base/res/ladybird/locale. Every catalog is checked whatever files are
# given, as a change to the source text can leave a translation without its text.
#
# Each catalog file is a scope, read only together with some others: the browser controls read chrome, actions,
# widgets and qt, and each of the browser's pages reads chrome and its own file. The same text may be in two files that
# are never read together, but not in two that are.

import json
import os
import re
import sys

from pathlib import Path
from typing import Dict
from typing import List
from typing import Set
from typing import Tuple

LOCALE_DIRECTORY = Path("Base/res/ladybird/locale")
PLURAL_CATEGORIES = {"zero", "one", "two", "few", "many", "other"}
RE_PLACEHOLDER = re.compile(r"\{\{|\}\}|\{\d\}|%\d|%n")
RE_LINE_COMMENT = re.compile(r"(^|\s)//.*$", re.MULTILINE)

CONTROL_SOURCES = "controls"
PAGE_SOURCES = "pages"
SOURCES: Dict[str, List[Tuple[Path, Set[str]]]] = {
    CONTROL_SOURCES: [(Path("UI/Qt"), {".cpp", ".h", ".mm"}), (Path("Libraries/LibWebView"), {".cpp", ".h", ".mm"})],
    PAGE_SOURCES: [(Path("Base/res/ladybird/about-pages"), {".html", ".js"})],
}

# Where the text of each file is used.
SURFACES: Dict[str, List[str]] = {
    "chrome": [CONTROL_SOURCES, PAGE_SOURCES],
    "actions": [CONTROL_SOURCES],
    "widgets": [CONTROL_SOURCES],
    "qt": [CONTROL_SOURCES],
    "settings": [PAGE_SOURCES],
    "pages": [PAGE_SOURCES],
}

# The files that are read together.
LOAD_SETS: List[Set[str]] = [{"chrome", "actions", "widgets", "qt"}, {"chrome", "settings"}, {"chrome", "pages"}]


def placeholders(text: str) -> List[str]:
    return sorted(token for token in RE_PLACEHOLDER.findall(text) if token not in ("{{", "}}"))


def translated_forms(value: object) -> Tuple[List[str], List[str]]:
    errors: List[str] = []
    if isinstance(value, str):
        return [value], errors
    if not isinstance(value, dict):
        return [], ["is neither a string nor an object of plural forms"]

    forms: List[str] = []
    for category, form in value.items():
        if category not in PLURAL_CATEGORIES:
            errors.append(f"has the unknown plural category '{category}'")
        elif not isinstance(form, str):
            errors.append(f"has a '{category}' form that is not a string")
        else:
            forms.append(form)
    if not isinstance(value.get("other"), str):
        errors.append("has no 'other' plural form")
    return forms, errors


def read_sources(name: str) -> str:
    contents: List[str] = []
    for directory, extensions in SOURCES[name]:
        for path in sorted(directory.rglob("*")):
            if path.suffix in extensions:
                # Text quoted in a comment isn't a use of it.
                text = path.read_text(encoding="utf-8", errors="replace")
                contents.append(RE_LINE_COMMENT.sub("", text))
    # Text in HTML may wrap across lines.
    return re.sub(r"\s+", " ", "\n".join(contents))


def is_used(source: str, text: str) -> bool:
    # The whole text must be a string literal, an attribute value or an element's text, not part of a longer text.
    pattern = "[\"'`>] ?" + r"\s+".join(map(re.escape, source.split())) + " ?[\"'`<]"
    return re.search(pattern, text) is not None


def reject_duplicate_keys(pairs: List[Tuple[str, object]]) -> Dict[str, object]:
    result: Dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"the key '{key}' is there more than once")
        result[key] = value
    return result


def run() -> int:
    errors: List[str] = []
    entries: Dict[Tuple[str, str, str, str], Path] = {}
    sources = {name: read_sources(name) for name in SOURCES}

    for path in sorted(LOCALE_DIRECTORY.glob("*/*.json")):
        language, surface = path.parent.name, path.stem
        if surface not in SURFACES:
            errors.append(f"{path}: is not one of the catalog files: {', '.join(SURFACES)}")
            continue
        try:
            catalog = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicate_keys)
        except (UnicodeDecodeError, ValueError) as error:
            errors.append(f"{path}: is not valid JSON: {error}")
            continue
        if not isinstance(catalog, dict):
            errors.append(f"{path}: must be an object of contexts")
            continue

        for context, messages in catalog.items():
            if not isinstance(messages, dict):
                errors.append(f"{path}: context '{context}' must be an object of messages")
                continue
            for source, value in messages.items():
                where = f"{path}: '{source}'" + (f" in context '{context}'" if context else "")
                entries[(language, surface, context, source)] = path

                forms, form_errors = translated_forms(value)
                errors.extend(f"{where} {error}" for error in form_errors)
                expected = placeholders(source)
                for form in forms:
                    if placeholders(form) != expected:
                        errors.append(f"{where} has the placeholders {placeholders(form)} instead of {expected}")

                if not any(is_used(source, sources[name]) for name in SURFACES[surface]):
                    places = ", ".join(str(directory) for name in SURFACES[surface] for directory, _ in SOURCES[name])
                    errors.append(f"{where} is not used anywhere in {places}")

    for (language, surface, context, source), path in entries.items():
        for other_surface in sorted(set().union(*(load_set for load_set in LOAD_SETS if surface in load_set))):
            if other_surface > surface and (language, other_surface, context, source) in entries:
                other_path = entries[(language, other_surface, context, source)]
                errors.append(f"{path}: '{source}' is also in {other_path}, which is read along with it")

    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__) + "/../..")
    sys.exit(run())
