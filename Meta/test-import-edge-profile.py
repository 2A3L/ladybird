#!/usr/bin/env python3
# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import importlib.util
import json
import sqlite3
import struct
import sys
import tempfile
import unittest
from contextlib import closing
from pathlib import Path


spec = importlib.util.spec_from_file_location("import_edge_profile", Path(__file__).with_name("import-edge-profile.py"))
importer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = importer
spec.loader.exec_module(importer)


def command(kind, payload=b""):
    return struct.pack("<HB", len(payload) + 1, kind) + payload


def navigate(tab, index, url):
    data = url.encode("utf-8")
    payload = struct.pack("<iii", tab, index, len(data)) + data + b"\0" * (-len(data) % 4)
    return command(6, struct.pack("<I", len(payload)) + payload)


def pair(kind, first, second):
    return command(kind, struct.pack("<ii", first, second))


class EdgeProfileImportTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.edge = self.root / "edge"
        self.template = self.root / "schema"
        self.destination = self.root / "output"
        (self.edge / "Sessions").mkdir(parents=True)
        self.template.mkdir()
        self.session = self.edge / "Sessions/Session_123"
        self.session.write_bytes(
            b"SNSS\x03\0\0\0"
            + pair(0, 1, 11) + pair(0, 1, 12) + pair(0, 2, 13) + pair(0, 3, 14)
            + pair(2, 11, 1) + pair(2, 12, 0) + pair(8, 1, 1)
            + command(12, struct.pack("<i?3x", 12, True))
            + navigate(11, 0, "https://example.com/first")
            + navigate(11, 1, "https://example.com/selected")
            + navigate(11, 2, "https://example.com/wrong")
            + pair(7, 11, 1)
            + navigate(12, 0, "https://example.net/")
            + navigate(13, 0, "https://example.org/")
            + navigate(14, 0, "https://closed.example/")
            + command(255)
            + command(17, struct.pack("<i4xq", 3, 0))
            + command(20, struct.pack("<i", 1))
        )
        epoch = importer.WINDOWS_TO_UNIX_MICROSECONDS
        (self.edge / "Bookmarks").write_text(json.dumps({"roots": {
            "bookmark_bar": {"type": "folder", "children": [
                {"type": "folder", "name": "folder", "date_added": str(epoch + 1_000), "children": [
                    {"type": "url", "name": "first", "url": "https://example.com/first"},
                    {"type": "url", "name": "duplicates are kept", "url": "https://example.com/first"},
                ]},
                {"type": "url", "name": "skip executable bookmark", "url": "javascript:void(0)"},
            ]},
            "other": {"type": "folder", "name": "other", "children": [
                {"type": "url", "name": "second", "url": "http://example.net/"}
            ]},
        }}), encoding="utf-8")
        with closing(sqlite3.connect(self.edge / "History")) as source, source:
            source.execute("CREATE TABLE urls(url, title, visit_count, typed_count, last_visit_time, hidden)")
            source.executemany("INSERT INTO urls VALUES (?, ?, ?, ?, ?, ?)", (
                ("https://example.com/first#one", "old", 3, 2, epoch + 1_000, 0),
                ("https://example.com/first#two", "new", 4, 1, epoch + 2_000, 0),
                ("https://hidden.example/", "hidden", 1, 0, epoch, 1),
                ("file:///etc/passwd", "local", 1, 0, epoch, 0),
            ))
        # Reuse the destination's actual SQL migrations from source for tests.
        repository = Path.cwd()
        for filename, source, named_version in (
            ("History.db", "Libraries/LibWebView/HistoryStore.cpp", ("History", 4)),
            ("Sessions.db", "UI/Qt/SessionStore.cpp", ("Sessions", 12)),
        ):
            with closing(sqlite3.connect(self.template / filename)) as connection, connection:
                connection.execute("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL)")
                connection.execute("INSERT INTO SchemaVersions VALUES (?, ?)", named_version)
                if filename == "History.db":
                    connection.execute("INSERT INTO SchemaVersions VALUES ('Favicons', 1)")
                migrations = (repository / source).read_text().split('auto migrations = to_array<Database::Migration>({', 1)[1]
                for section in migrations.split('});', 1)[0].split('R"#(')[1:]:
                    connection.executescript(section.split(')#"sv')[0])
                if filename == "History.db":
                    connection.execute("INSERT INTO History (url,title,visit_count,last_visited_time) VALUES ('https://template.example/', 'private', 1, 0)")

    def test_migration_is_ordered_and_isolated(self):
        counts = importer.migrate(self.edge, self.template, self.destination, None)
        self.assertEqual(counts["tabs"], 3)
        self.assertEqual(counts["bookmarks"], 3)
        self.assertEqual(counts["skipped_bookmarks"], 1)
        self.assertEqual(counts["history_entries"], 1)
        self.assertEqual(counts["history_rows_merged"], 1)
        self.assertEqual(counts["skipped_history"], 1)
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o700)
        with closing(sqlite3.connect(self.destination / "data/Sessions.db")) as target:
            self.assertEqual(target.execute("SELECT id, active_tab_index FROM Sessions ORDER BY id").fetchall(), [(1, 0), (2, 1)])
            self.assertEqual(target.execute("SELECT active_url, pinned FROM SessionTabs ORDER BY id").fetchall(), [
                ("https://example.org/", 0), ("https://example.net/", 1), ("https://example.com/selected", 0),
            ])
        with closing(sqlite3.connect(self.destination / "data/History.db")) as target:
            self.assertEqual(target.execute("SELECT url,title,visit_count,direct_visit_count,last_visited_time FROM History").fetchall(), [
                ("https://example.com/first", "new", 7, 3, 2),
            ])
        bookmarks = json.loads((self.destination / "config/Bookmarks.json").read_text())["items"]
        self.assertEqual([item["title"] for item in bookmarks], ["folder", "other"])
        self.assertEqual([item["title"] for item in bookmarks[0]["children"]], ["first", "duplicates are kept"])
        with self.assertRaises(FileExistsError):
            importer.migrate(self.edge, self.template, self.destination, None)

    def test_invalid_session_does_not_publish_a_profile(self):
        with self.session.open("ab") as source:
            source.write(b"\xff")
        with self.assertRaises(ValueError):
            importer.migrate(self.edge, self.template, self.destination, None)
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main()
