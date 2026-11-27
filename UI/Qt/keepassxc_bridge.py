# SPDX-License-Identifier: BSD-2-Clause

"""One consented browser action per process. Input/output are private pipes."""
import base64
import ctypes
import ctypes.util
import hashlib
import json
import os
import resource
import signal
import struct
import subprocess
import sys
import time
from urllib.parse import urlsplit

def b64(data):
    return base64.b64encode(data).decode("ascii")

def un64(data):
    return base64.b64decode(data, validate=True)

def next_nonce(nonce):
    return ((int.from_bytes(nonce, "little") + 1) % (1 << 192)).to_bytes(24, "little")

class Failure(Exception):
    pass

class Unavailable(Failure):
    pass

class Refused(Failure):
    def __init__(self, code):
        self.code = str(code)
        super().__init__({"1": "Unlock the database in KeePassXC.", "5": "Open KeePassXC and enable browser integration.",
                          "6": "KeePassXC cancelled the operation.", "8": "The association does not match the open database."}.get(self.code, "KeePassXC declined the operation."))

class Box:
    def __init__(self):
        self.lib = ctypes.CDLL(ctypes.util.find_library("sodium") or "libsodium.so")
        if self.lib.sodium_init() < 0:
            raise Failure("Unable to initialize libsodium.")
        self.pk, self.sk = ctypes.create_string_buffer(32), ctypes.create_string_buffer(32)
        if self.lib.crypto_box_keypair(self.pk, self.sk) != 0:
            raise Failure("Unable to initialize the encrypted channel.")

    def crypt(self, data, nonce, peer, decrypt=False):
        if len(nonce) != 24 or len(peer) != 32 or (decrypt and len(data) < 16):
            raise Failure("Invalid response from KeePassXC.")
        size = len(data) - 16 if decrypt else len(data) + 16
        out = ctypes.create_string_buffer(size)
        f = self.lib.crypto_box_open_easy if decrypt else self.lib.crypto_box_easy
        if f(out, data, ctypes.c_ulonglong(len(data)), nonce, peer, self.sk) != 0:
            raise Failure("Could not authenticate the KeePassXC message.")
        return out.raw

class Session:
    def __init__(self, pipe):
        self.pipe = pipe
        self.box = Box()
        self.client = b64(os.urandom(24))
        self.unlocked = False
        n = os.urandom(24)
        reply = self.exchange("change-public-keys", n, publicKey=b64(self.box.pk.raw))
        self.peer = un64(reply["publicKey"])

    def receive(self):
        header = self.pipe.stdout.read(4)
        if len(header) != 4:
            raise Unavailable("Open KeePassXC and enable browser integration.")
        size, = struct.unpack("=I", header)
        if not 0 < size <= 1024 * 1024:
            raise Failure("The KeePassXC response is too large.")
        reply = json.loads(self.pipe.stdout.read(size))
        if reply.get("action") in ("database-locked", "database-unlocked"):
            self.unlocked = reply["action"] == "database-unlocked"
        return reply

    def exchange(self, action, n, **fields):
        payload = json.dumps(dict(action=action, nonce=b64(n), clientID=self.client, **fields)).encode()
        self.pipe.stdin.write(struct.pack("=I", len(payload)) + payload)
        self.pipe.stdin.flush()
        while True:
            reply = self.receive()
            if reply.get("action") != action:
                continue
            if "errorCode" in reply:
                raise Refused(reply["errorCode"])
            if reply.get("nonce") != b64(next_nonce(n)):
                raise Failure("The KeePassXC reply does not match the request.")
            return reply

    def call(self, action, *, unlock=False, **fields):
        n = os.urandom(24)
        data = json.dumps(dict(action=action, **fields)).encode()
        header = {"triggerUnlock": "true"} if unlock else {}
        reply = self.exchange(action, n, message=b64(self.box.crypt(data, n, self.peer)), **header)
        data = json.loads(self.box.crypt(un64(reply["message"]), next_nonce(n), self.peer, True))
        if data.get("nonce") != b64(next_nonce(n)) or data.get("success") not in ("true", True) or data.get("error") not in (None, "", "success"):
            raise Failure("KeePassXC did not confirm the operation.")
        return data

    def request_unlock(self):
        self.unlocked = False
        try:
            self.call("get-databasehash", unlock=True)
        except Refused as e:
            if e.code != "1":
                raise
            while not self.unlocked:
                self.receive()

def keyring(mode, profile, value=None):
    attrs = ["application", "org.ladybird.Ladybird", "profile", hashlib.sha256(os.fsencode(os.path.realpath(profile))).hexdigest()]
    cmd = ["/usr/bin/secret-tool", mode]
    if mode == "store":
        cmd += ["--label=Ladybird KeePassXC association"]
    p = subprocess.run(cmd + attrs, input=json.dumps(value).encode() if value is not None else b"",
                       capture_output=True, timeout=75, check=False)
    if mode == "lookup" and p.returncode == 1 and not p.stderr.strip():
        return None
    if p.returncode:
        raise Failure("Unable to access your session keyring.")
    return json.loads(p.stdout) if mode == "lookup" else None

def operate(s, req, store=keyring):
    site = req["origin"]
    url = urlsplit(site)
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.path or url.query or url.fragment:
        raise Failure("An HTTPS origin is required.")
    if req.get("unlock") is True:
        s.request_unlock()
    key = store("lookup", req["profile"])
    if not key:
        ident = b64(Box().pk.raw)
        reply = s.call("associate", key=b64(s.box.pk.raw), idKey=ident)
        key = dict(id=reply["id"], key=ident)
        store("store", req["profile"], key)
    else:
        s.call("test-associate", **key)
    def entries():
        try:
            return s.call("get-logins", url=site, submitUrl=site, id=key["id"], keys=[key])["entries"]
        except Refused as e:
            if e.code == "15":
                return []
            raise
    items = entries()
    op = req["op"]
    if op == "list":
        return [dict(uuid=i["uuid"], name=i.get("name", ""), login=i["login"]) for i in items]
    uid = req.get("uuid")
    if op in ("fill", "update"):
        selected = next((i for i in items if i["uuid"] == uid and uid), None)
        if not selected:
            raise Failure("The selected account is no longer available.")
        if op == "fill":
            return {k: selected[k] for k in ("login", "password")}
    if op not in ("new", "update") or not req.get("password"):
        raise Failure("Enter a password to save.")
    s.call("set-login", url=site, submitUrl=site, id=key["id"], login=req["login"], password=req["password"], uuid=uid if op == "update" else "")
    previous = {i["uuid"] for i in items}
    fresh = entries()
    if not any((i["uuid"] == uid if op == "update" else i["uuid"] not in previous) and
               i["login"] == req["login"] and i["password"] == req["password"] for i in fresh):
        raise Failure("KeePassXC received the request. Check the entry before repeating, because the save could not be confirmed.")
    return True

def main():
    def cancelled(*_):
        raise SystemExit(1)
    signal.signal(signal.SIGTERM, cancelled)
    def timeout(*_):
        raise Failure("The KeePassXC request timed out.")
    def unavailable(*_):
        raise Unavailable("KeePassXC is not connected yet.")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    req = json.loads(sys.stdin.buffer.read(1024 * 1024))
    for attempt in range(11):
        with subprocess.Popen(["/usr/bin/keepassxc-proxy"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL) as pipe:
            try:
                # The native proxy may stay open without a connected KeePassXC.
                signal.signal(signal.SIGALRM, unavailable)
                signal.setitimer(signal.ITIMER_REAL, 0.6)
                try:
                    session = Session(pipe)
                except (BrokenPipeError, Unavailable):
                    if req.get("unlock") is not True or attempt == 10:
                        raise Unavailable("Enable browser integration in KeePassXC.")
                else:
                    signal.signal(signal.SIGALRM, timeout)
                    signal.alarm(120)
                    print(json.dumps({"value": operate(session, req)}))
                    return
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
                pipe.kill()
        if attempt == 0:
            args = ["/usr/bin/keepassxc"]
            database = req.get("database")
            if database:
                if not os.path.isabs(database) or not os.path.isfile(database):
                    raise Failure("Select a valid KeePassXC database.")
                args.append(database)
            subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        time.sleep(0.2)

if __name__ == "__main__":
    try:
        main()
    except Failure as e:
        print(json.dumps({"error": str(e)}))
    except (Exception, KeyboardInterrupt):
        print(json.dumps({"error": "The KeePassXC operation could not be completed."}))
