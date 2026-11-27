# SPDX-License-Identifier: BSD-2-Clause

"""A framed, encrypted in-memory KeePassXC peer; never connects to the desktop vault."""
import importlib.util
import io
import json
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("bridge", Path(__file__).resolve().parents[3] / "UI/Qt/keepassxc_bridge.py")
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

class Peer:
    def __init__(self):
        self.box = b.Box()
        self.stdin = self
        self.stdout = io.BytesIO()
        self.items = {}
        self.bad_mac = False
        self.locked = False
        self.allow_unlock = False

    def flush(self):
        pass

    def write(self, framed):
        size, = struct.unpack("=I", framed[:4])
        assert size == len(framed) - 4
        msg = json.loads(framed[4:])
        n = b.un64(msg["nonce"])
        reply_nonce = b.next_nonce(n)
        action = msg["action"]
        reply = {"action": action, "nonce": b.b64(reply_nonce)}
        if action == "change-public-keys":
            self.client = b.un64(msg["publicKey"])
            reply["publicKey"] = b.b64(self.box.pk.raw)
        elif self.locked:
            reply["errorCode"] = 1
        else:
            inner = json.loads(self.box.crypt(b.un64(msg["message"]), n, self.client, True))
            assert inner["action"] == action
            data = {"success": "true", "nonce": reply["nonce"]}
            if action == "get-databasehash":
                assert msg["triggerUnlock"] == "true"
            elif action == "associate":
                data["id"] = "Ladybird Test"
                self.idkey = inner["idKey"]
            elif action == "test-associate":
                assert self.idkey == inner["key"] and inner["id"] == "Ladybird Test"
            elif action == "get-logins":
                assert inner["keys"] == [{"id":"Ladybird Test", "key":self.idkey}]
                data["entries"] = self.items.get(inner["url"], [])
            elif action == "set-login":
                items = self.items.setdefault(inner["url"], [])
                if inner["uuid"]:
                    i = next(x for x in items if x["uuid"] == inner["uuid"])
                else:
                    i = {"uuid": str(len(items) + 1), "name": "Test"}
                    items.append(i)
                i.update(login=inner["login"], password=inner["password"])
            else:
                raise AssertionError(action)
            enc = self.box.crypt(json.dumps(data).encode(), reply_nonce, self.client)
            if self.bad_mac:
                enc = bytes([enc[0] ^ 1]) + enc[1:]
            reply["message"] = b.b64(enc)
        result = json.dumps(reply).encode()
        framed_reply = struct.pack("=I", len(result)) + result
        if self.locked and self.allow_unlock and msg.get("triggerUnlock") == "true":
            assert action == "get-databasehash"
            self.locked = False
            signal = b'{"action":"database-unlocked"}'
            framed_reply += struct.pack("=I", len(signal)) + signal
        self.stdout = io.BytesIO(framed_reply)

class TestBridge(unittest.TestCase):
    def test_lifecycle_separate_sessions(self):
        peer = Peer()
        cache = {}
        def store(mode, profile, data=None):
            if mode == "lookup":
                return cache.get(profile)
            cache[profile] = data
        def request(op, site="https://library.example", **kw):
            return b.operate(b.Session(peer), dict(op=op, origin=site, profile="test", **kw), store)
        self.assertEqual(request("list"), [])
        self.assertTrue(request("new", login="reader", password="dummy-one"))
        accounts = request("list")
        self.assertNotIn("password", accounts[0])
        uid = accounts[0]["uuid"]
        self.assertEqual(request("fill", uuid=uid)["password"], "dummy-one")
        self.assertTrue(request("update", uuid=uid, login="reader", password="dummy-two"))
        self.assertEqual(request("fill", uuid=uid)["password"], "dummy-two")
        with self.assertRaises(b.Failure):
            request("fill", "https://elsewhere.example", uuid=uid)
        with self.assertRaises(b.Failure):
            request("list", "http://library.example")
        peer.locked = True
        with self.assertRaises(b.Refused):
            request("fill", uuid=uid)
        peer.allow_unlock = True
        self.assertEqual(request("fill", uuid=uid, unlock=True)["password"], "dummy-two")
        peer.locked = False
        peer.bad_mac = True
        with self.assertRaises(b.Failure):
            request("fill", uuid=uid)

if __name__ == "__main__":
    unittest.main()
