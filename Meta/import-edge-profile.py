#!/usr/bin/env python3
# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Import an Edge profile into a new self-contained Ladybird Qt profile on Linux."""

import argparse
import ctypes
import json
import os
import sqlite3
import struct
import tempfile
import uuid
from collections import defaultdict
from contextlib import closing
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from urllib.parse import urldefrag, urlsplit


# Chromium's base::Time epoch is 1601-01-01 UTC.
WINDOWS_TO_UNIX_MICROSECONDS = 11_644_473_600_000_000


class Command(IntEnum):
    SET_TAB_WINDOW = 0
    SET_TAB_INDEX = 2
    PRUNE_FROM_BACK = 5
    UPDATE_NAVIGATION = 6
    SELECT_NAVIGATION = 7
    SELECT_TAB = 8
    PRUNE_FROM_FRONT = 11
    SET_PINNED = 12
    CLOSE_TAB = 16
    CLOSE_WINDOW = 17
    ACTIVATE_WINDOW = 20
    PRUNE_NAVIGATION = 24
    INITIAL_STATE_MARKER = 255


@dataclass
class Tab:
    window_id: int | None = None
    position: int = -1
    current_navigation: int = -1
    pinned: bool = False
    navigations: dict = field(default_factory=dict)


def unix_milliseconds(value):
    return max(0, (int(value or 0) - WINDOWS_TO_UNIX_MICROSECONDS) // 1000)


def is_web_url(value):
    if not isinstance(value, str) or "\\" in value or any(ord(character) <= 32 for character in value):
        return False
    try:
        url = urlsplit(value)
        return (
            url.scheme in ("http", "https")
            and bool(url.hostname)
            and url.username is None
            and url.password is None
            and (url.port is None or 0 <= url.port <= 65535)
        )
    except ValueError:
        return False


def bookmarks_from_edge(path):
    roots = json.loads(path.read_text(encoding="utf-8"))["roots"]
    counts = {"bookmarks": 0, "folders": 0, "skipped_bookmarks": 0}

    def convert(node):
        item = {
            "id": str(uuid.uuid4()),
            "title": node.get("name", ""),
            "dateAdded": unix_milliseconds(node.get("date_added")),
            "lastModified": unix_milliseconds(node.get("date_modified") or node.get("date_added")),
        }
        if node["type"] == "folder":
            children = [converted for child in node.get("children", []) if (converted := convert(child)) is not None]
            counts["folders"] += 1
            return dict(item, type="folder", children=children)
        if node["type"] == "url" and is_web_url(node.get("url")):
            counts["bookmarks"] += 1
            return dict(item, type="bookmark", url=node["url"])
        counts["skipped_bookmarks"] += 1
        return None

    # Ladybird's root is its bookmarks bar. Keep other Edge roots as folders.
    items = [item for child in roots["bookmark_bar"]["children"] if (item := convert(child)) is not None]
    for key, root in roots.items():
        # Some Chromium profiles also include root metadata next to the folders.
        if key != "bookmark_bar" and isinstance(root, dict) and root.get("type") == "folder":
            items.append(convert(root))
    return {"version": 2, "items": items}, counts


def session_commands(path):
    # https://chromium.googlesource.com/chromium/src/+/main/components/sessions/core/command_storage_backend.cc
    # shortcut: Only unencrypted SNSS v3 is supported. Use Edge's export UI if the profile uses encrypted sessions.
    with path.open("rb") as stream:
        if stream.read(8) != b"SNSS" + struct.pack("<I", 3):
            raise ValueError("Unsupported Edge session format (expected unencrypted SNSS v3)")
        while prefix := stream.read(2):
            if len(prefix) != 2:
                raise ValueError("The Edge session ends with a truncated record length")
            (size,) = struct.unpack("<H", prefix)
            record = stream.read(size)
            if not record or len(record) != size:
                raise ValueError("The Edge session ends with a truncated record")
            yield record[0], record[1:]


def prune_navigations(tab, start, count):
    if start < 0 or count <= 0:
        raise ValueError("Invalid Edge navigation pruning range")
    end = start + count
    tab.navigations = {
        index - (count if index >= end else 0): url
        for index, url in tab.navigations.items()
        if not start <= index < end
    }
    if tab.current_navigation >= end:
        tab.current_navigation -= count
    elif tab.current_navigation >= start:
        tab.current_navigation = start - 1


def windows_from_edge(path):
    # https://chromium.googlesource.com/chromium/src/+/main/components/sessions/core/session_service_commands.cc
    # Only the IDs and current GET URLs are used. Encoded page state is left in Edge.
    tabs = defaultdict(Tab)
    selected_tabs = {}
    closed_windows = set()
    active_window = None
    has_initial_state = False
    for command, data in session_commands(path):
        if command == Command.SET_TAB_WINDOW:
            window_id, tab_id = struct.unpack("<ii", data)
            tabs[tab_id].window_id = window_id
        elif command == Command.SET_TAB_INDEX:
            tab_id, position = struct.unpack("<ii", data)
            tabs[tab_id].position = position
        elif command == Command.UPDATE_NAVIGATION:
            pickle_size, tab_id, index, url_length = struct.unpack_from("<Iiii", data)
            if pickle_size != len(data) - 4 or not 0 <= url_length <= len(data) - 16:
                raise ValueError("Invalid Edge navigation record")
            tabs[tab_id].navigations[index] = data[16 : 16 + url_length].decode("utf-8")
        elif command == Command.SELECT_NAVIGATION:
            tab_id, index = struct.unpack("<ii", data)
            tabs[tab_id].current_navigation = index
        elif command == Command.SELECT_TAB:
            window_id, index = struct.unpack("<ii", data)
            selected_tabs[window_id] = index
        elif command == Command.SET_PINNED:
            tab_id, pinned = struct.unpack("<i?3x", data)
            tabs[tab_id].pinned = pinned
        elif command == Command.CLOSE_TAB:
            (tab_id,) = struct.unpack_from("<i", data)
            tabs.pop(tab_id, None)
        elif command == Command.CLOSE_WINDOW:
            (window_id,) = struct.unpack_from("<i", data)
            closed_windows.add(window_id)
        elif command == Command.ACTIVATE_WINDOW:
            (active_window,) = struct.unpack("<i", data)
        elif command == Command.PRUNE_NAVIGATION:
            tab_id, index, count = struct.unpack("<iii", data)
            prune_navigations(tabs[tab_id], index, count)
        elif command == Command.PRUNE_FROM_BACK:
            tab_id, start = struct.unpack("<ii", data)
            tab = tabs[tab_id]
            tab.navigations = {index: url for index, url in tab.navigations.items() if index < start}
        elif command == Command.PRUNE_FROM_FRONT:
            tab_id, count = struct.unpack("<ii", data)
            prune_navigations(tabs[tab_id], 0, count)
        elif command == Command.INITIAL_STATE_MARKER:
            has_initial_state = True
    if not has_initial_state:
        raise ValueError("The Edge session lacks a complete initial snapshot")

    windows = defaultdict(list)
    skipped = 0
    for tab in tabs.values():
        if tab.window_id is None or tab.window_id in closed_windows:
            continue
        if not tab.navigations:
            skipped += 1
            continue
        last_index = max(tab.navigations)
        index = min((index for index in tab.navigations if index >= tab.current_navigation), default=last_index)
        url = tab.navigations[index]
        if url in ("edge://newtab/", "chrome://newtab/"):
            url = "about:newtab"
        elif not is_web_url(url):
            skipped += 1
            continue
        windows[tab.window_id].append((tab.position, url, tab.pinned))

    imported = []
    for window_id in sorted(windows, key=lambda window_id: (window_id == active_window, window_id)):
        ordered = sorted(windows[window_id], key=lambda tab: tab[0])
        selected = selected_tabs.get(window_id, 0)
        selected_index = next((index for index, tab in enumerate(ordered) if tab[0] >= selected), len(ordered) - 1)
        imported.append((selected_index, ordered))
    return imported, skipped


def read_only(path, *, immutable=False):
    mode = "&immutable=1" if immutable else ""
    connection = sqlite3.connect(f"{path.as_uri()}?mode=ro{mode}", uri=True)
    connection.execute("PRAGMA trusted_schema = OFF")
    return closing(connection)


def initialize_schema(source, target, expected_versions):
    # Copy only DDL and version metadata, never a template profile's rows.
    with read_only(source, immutable=True) as template, target:
        versions = dict(template.execute("SELECT store, version FROM SchemaVersions"))
        if versions != expected_versions:
            raise ValueError(f"{source.name} uses an incompatible Ladybird schema version")
        for (ddl,) in template.execute(
            """SELECT sql FROM sqlite_schema
               WHERE type IN ('table', 'index') AND sql IS NOT NULL AND name NOT LIKE 'sqlite_%'
               ORDER BY type DESC"""
        ):
            if not ddl.startswith(("CREATE TABLE ", "CREATE INDEX ", "CREATE UNIQUE INDEX ")):
                raise ValueError(f"{source.name} contains unexpected schema definitions")
            target.execute(ddl)
        target.executemany("INSERT INTO SchemaVersions(store, version) VALUES (?, ?)", versions.items())


def import_history(source, target):
    rows = skipped = 0
    with read_only(source) as edge, target:
        for url, title, count, typed, visited in edge.execute(
            "SELECT url, title, visit_count, typed_count, last_visit_time FROM urls WHERE hidden = 0 AND visit_count > 0"
        ):
            if not is_web_url(url):
                skipped += 1
                continue
            visited_at = unix_milliseconds(visited)
            target.execute(
                """INSERT INTO History
                       (url, title, visit_count, last_visited_time, direct_visit_count,
                        last_qualifying_visit_time, last_direct_visit_time, decayed_visit_score,
                        decayed_direct_score, score_updated_at)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                   ON CONFLICT(url) DO UPDATE SET
                       title = CASE WHEN excluded.last_visited_time >= last_visited_time AND excluded.title != ''
                                    THEN excluded.title ELSE title END,
                       visit_count = visit_count + excluded.visit_count,
                       last_visited_time = max(last_visited_time, excluded.last_visited_time),
                       direct_visit_count = direct_visit_count + excluded.direct_visit_count,
                       last_qualifying_visit_time = max(last_qualifying_visit_time, excluded.last_qualifying_visit_time),
                       last_direct_visit_time = max(last_direct_visit_time, excluded.last_direct_visit_time),
                       decayed_visit_score = min(8.0, decayed_visit_score + excluded.decayed_visit_score),
                       decayed_direct_score = min(8.0, decayed_direct_score + excluded.decayed_direct_score),
                       score_updated_at = max(score_updated_at, excluded.score_updated_at)""",
                (
                    urldefrag(url).url, title or "", count, visited_at, typed,
                    visited_at, visited_at if typed else 0, min(8, count), min(8, typed), visited_at,
                ),
            )
            rows += 1
    unique = target.execute("SELECT count(*) FROM History").fetchone()[0]
    return {"history_entries": unique, "history_rows_merged": rows - unique, "skipped_history": skipped}


def import_windows(target, windows):
    tab_count = 0
    with target:
        for window_id, (active_tab, tabs) in enumerate(windows, 1):
            target.execute(
                "INSERT INTO Sessions(id, kind, closed_time, active_tab_index, activation_sequence) VALUES (?, 0, 0, ?, ?)",
                (window_id, active_tab, window_id),
            )
            for position, (_, url, pinned) in enumerate(tabs):
                tab_count += 1
                target.execute(
                    """INSERT INTO SessionTabs
                           (id, session_id, tab_ordinal, active_url, current_used_step_index, pinned)
                       VALUES (?, ?, ?, ?, 0, ?)""",
                    (tab_count, window_id, position, url, pinned),
                )
    return {"windows": len(windows), "tabs": tab_count}


def publish(staging, destination):
    # Linux renameat2(RENAME_NOREPLACE) publishes the whole profile without a race
    # that could overwrite an existing path, including an empty directory.
    rename = ctypes.CDLL(None, use_errno=True).renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    if rename(-100, os.fsencode(staging), -100, os.fsencode(destination), 1) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), destination)


def migrate(edge, schema_directory, destination, session):
    if os.path.lexists(destination):
        raise FileExistsError("The destination already exists")
    if session is None:
        session = max(
            (path for path in (edge / "Sessions").glob("Session_*") if path.name[8:].isdigit()),
            key=lambda path: int(path.name[8:]),
            default=None,
        )
    if session is None:
        raise ValueError("No Edge Session_* snapshot found")

    bookmarks, counts = bookmarks_from_edge(edge / "Bookmarks")
    windows, counts["skipped_tabs"] = windows_from_edge(session)
    counts["session_snapshot"] = session.name

    with tempfile.TemporaryDirectory(prefix=".ladybird-edge-import-", dir=destination.parent) as temporary:
        stage = Path(temporary)
        (stage / "config").mkdir(mode=0o700)
        (stage / "data").mkdir(mode=0o700)
        (stage / "config/Bookmarks.json").write_text(json.dumps(bookmarks, ensure_ascii=False), encoding="utf-8")
        (stage / "config/Settings.json").write_text(
            json.dumps({"firstRunComplete": True, "restorePreviousSession": True}), encoding="utf-8"
        )
        for filename, versions in (("History.db", {"History": 4, "Favicons": 1}), ("Sessions.db", {"Sessions": 12})):
            with closing(sqlite3.connect(stage / "data" / filename)) as target:
                target.execute("PRAGMA trusted_schema = OFF")
                target.execute("PRAGMA foreign_keys = ON")
                initialize_schema(schema_directory / filename, target, versions)
                if filename == "History.db":
                    counts.update(import_history(edge / "History", target))
                else:
                    counts.update(import_windows(target, windows))
                if target.execute("PRAGMA quick_check").fetchone() != ("ok",):
                    raise ValueError(f"{filename} failed SQLite integrity verification")
                if target.execute("PRAGMA foreign_key_check").fetchone() is not None:
                    raise ValueError(f"{filename} failed SQLite foreign key verification")
        publish(stage, destination)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--edge-profile", type=Path, required=True, help="Edge profile directory; close Edge first")
    parser.add_argument(
        "--schema-directory", type=Path, required=True, help="Directory with History.db and Sessions.db from Ladybird"
    )
    parser.add_argument("--destination", type=Path, required=True, help="New directory to use with Ladybird --profile-path")
    parser.add_argument("--session", type=Path, help="Session_... snapshot to use (default: newest)")
    args = parser.parse_args()
    try:
        counts = migrate(
            args.edge_profile.expanduser().absolute(),
            args.schema_directory.expanduser().absolute(),
            args.destination.expanduser().absolute(),
            args.session.expanduser().absolute() if args.session else None,
        )
    except (OSError, ValueError, KeyError, TypeError, sqlite3.Error, struct.error) as error:
        parser.exit(1, f"Import failed: {error}\n")
    print(json.dumps(counts, indent=2))


if __name__ == "__main__":
    main()
