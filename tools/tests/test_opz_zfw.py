"""Tests for opz_zfw.py (decrypting the user's own .zfw package, offline).

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The AES cases check the known answers of FIPS-197 and NIST SP 800-38A. The package cases build a synthetic package
of our own, encrypted with a random test key generated here. The check against TE's 1.2.45 package is skipped
unless the package and a key file are in the repository (both git-ignored, never committed).
"""

import contextlib
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_zfw  # noqa: E402

TE_PACKAGE = os.path.join(REPO, "z_firmware_1_2_45.zfw")
KEYS = os.path.join(REPO, "keys")
TE_ZIP = os.path.join(REPO, "decrypted", "z_firmware_1_2_45.zip")


def h(text):
    return bytes.fromhex(text)


class KnownAnswers(unittest.TestCase):
    PLAIN = h("00112233 44556677 8899aabb ccddeeff")

    def test_fips197_appendix_c(self):
        for size, want in ((16, "69c4e0d8 6a7b0430 d8cdb780 70b4c55a"), (24, "dda97ca4 864cdfe0 6eaf70a0 ec0d7191"),
                           (32, "8ea2b7ca 516745bf eafc4990 4b496089")):
            with self.subTest(size=size):
                aes = opz_zfw.AES(bytes(range(size)))
                self.assertEqual(aes.encrypt_block(self.PLAIN), h(want))
                self.assertEqual(aes.decrypt_block(h(want)), self.PLAIN)

    def test_sp800_38a_cbc_aes256(self):
        k = h("603deb10 15ca71be 2b73aef0 857d7781 1f352c07 3b6108d7 2d9810a3 0914dff4")
        iv = bytes(range(16))
        plain = h("6bc1bee2 2e409f96 e93d7e11 7393172a ae2d8a57 1e03ac9c 9eb76fac 45af8e51"
                  "30c81c46 a35ce411 e5fbc119 1a0a52ef f69f2445 df4f9b17 ad2b417b e66c3710")
        cipher = h("f58c4c04 d6e5f1ba 779eabfb 5f7bfbd6 9cfc4e96 7edb808d 679f777b c6702c7d"
                   "39f23369 a9d9bacf a530e263 04231461 b2eb05e2 c39be9fc da6c1907 8c6a9d1b")
        self.assertEqual(opz_zfw.cbc_encrypt(k, iv, plain), cipher)
        self.assertEqual(opz_zfw.cbc_decrypt(k, iv, cipher), plain)

    def test_refuses_bad_sizes(self):
        with self.assertRaises(ValueError):
            opz_zfw.AES(bytes(31))
        with self.assertRaises(ValueError):
            opz_zfw.cbc_decrypt(bytes(32), bytes(16), bytes(15))


def make_zip(os_image=b"\x00OS" * 1000, compression=zipfile.ZIP_DEFLATED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression) as z:
        z.writestr(opz_zfw.OS_IMAGE, os_image)
        z.writestr("firmware/versions.txt", "test 0.0.1\n")
        z.writestr("firmware/blackfin/system/content/readme.txt", "x" * 5000)
    return buf.getvalue()


def make_package(zipped, k, name=b"firmware_test.zip"):
    """A package laid out like TE's: IV at 0x70, ZIP length at 0x80, one CBC stream from 0x300 (name, then ZIP)."""
    iv = os.urandom(16)
    head = bytearray(opz_zfw.NAME_AT)
    head[opz_zfw.IV_AT:opz_zfw.IV_AT + 16] = iv
    struct.pack_into("<I", head, opz_zfw.LENGTH_AT, len(zipped))
    pad = 16 - len(zipped) % 16
    stream = name.ljust(opz_zfw.NAME_LEN, b"\0") + zipped + bytes([pad]) * pad
    return bytes(head) + opz_zfw.cbc_encrypt(k, iv, stream)


def known(package, os_image):
    return [{"version": "0.0.1", "name": "test.zfw", "size": len(package),
             "sha256": hashlib.sha256(package).hexdigest(), "os_sha256": hashlib.sha256(os_image).hexdigest()}]


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = opz_zfw.main(argv)
    return rc, out.getvalue(), err.getvalue()


class Synthetic(unittest.TestCase):
    def setUp(self):
        self.key = os.urandom(32)
        self.os_image = os.urandom(3000)
        self.zipped = make_zip(self.os_image)
        self.package = make_package(self.zipped, self.key)
        self.known = known(self.package, self.os_image)

    def test_decrypts_and_checks(self):
        name, zipped = opz_zfw.decrypt(self.package, self.key)
        self.assertEqual(name, "firmware_test.zip")
        self.assertEqual(zipped, self.zipped)
        with mock.patch.object(opz_zfw, "KNOWN", self.known):
            entry = opz_zfw.identify(self.package)
        self.assertEqual(entry["version"], "0.0.1")
        lines = opz_zfw.check_zip(zipped, entry)
        self.assertIn("3 entries", lines[0])
        self.assertIn("SHA-256 matches 0.0.1", lines[1])

    def test_whole_block_zip_and_odd_name(self):
        zipped = make_zip(b"\x01" * 16)
        zipped += bytes(-len(zipped) % 16)           # trailing bytes after the ZIP are allowed by zipfile
        package = make_package(zipped, self.key, name=b"../\xffevil.zip")
        name, out = opz_zfw.decrypt(package, self.key)
        self.assertEqual(out, zipped)
        self.assertNotIn("/", name)

    def test_wrong_key_is_refused(self):
        with self.assertRaisesRegex(opz_zfw.ZfwError, "not a ZIP"):
            opz_zfw.decrypt(self.package, os.urandom(32))

    def test_damaged_entry_fails_the_crc(self):
        zipped = make_zip(self.os_image, zipfile.ZIP_STORED)
        package = bytearray(make_package(zipped, self.key))
        at = opz_zfw.ZIP_AT + zipped.index(self.os_image) + 1000
        package[at] ^= 0x01                          # flips one bit of the next plaintext block
        _, out = opz_zfw.decrypt(bytes(package), self.key)
        with self.assertRaisesRegex(opz_zfw.ZfwError, "CRC"):
            opz_zfw.check_zip(out, None)

    def test_os_image_hash_and_presence(self):
        entry = dict(self.known[0], os_sha256=hashlib.sha256(b"other").hexdigest())
        with self.assertRaisesRegex(opz_zfw.ZfwError, "SHA-256"):
            opz_zfw.check_zip(self.zipped, entry)
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("firmware/versions.txt", "x")
        with self.assertRaisesRegex(opz_zfw.ZfwError, "missing"):
            opz_zfw.check_zip(buf.getvalue(), entry)

    def test_header_checks(self):
        with self.assertRaisesRegex(opz_zfw.ZfwError, "not a package"):
            opz_zfw.header(self.package[:-1])
        bad = bytearray(self.package)
        struct.pack_into("<I", bad, opz_zfw.LENGTH_AT, len(self.package))
        with self.assertRaisesRegex(opz_zfw.ZfwError, "length"):
            opz_zfw.header(bytes(bad))

    def test_key_file_forms(self):
        with tempfile.TemporaryDirectory() as d:
            raw, text, bad = (os.path.join(d, n) for n in ("raw", "text", "bad"))
            with open(raw, "wb") as f:
                f.write(self.key)
            with open(text, "w") as f:
                f.write(self.key.hex().upper() + "\n\nnotes after the first line are ignored\n")
            with open(bad, "w") as f:
                f.write(self.key.hex()[:-2] + "\n")
            self.assertEqual(opz_zfw.read_key(raw), self.key)
            self.assertEqual(opz_zfw.read_key(text), self.key)
            with self.assertRaisesRegex(opz_zfw.ZfwError, "64 hex digits"):
                opz_zfw.read_key(bad)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            pkg, kf, out = os.path.join(d, "test.zfw"), os.path.join(d, "k"), os.path.join(d, "out")
            with open(pkg, "wb") as f:
                f.write(self.package)
            with open(kf, "w") as f:
                f.write(self.key.hex() + "\n")
            rc, _, err = run([pkg, "--key-file", kf, "--out", out])
            self.assertEqual(rc, 1)
            self.assertIn("not a known package", err)
            with mock.patch.object(opz_zfw, "KNOWN", self.known):
                rc, text, _ = run([pkg, "--info"])
                self.assertEqual(rc, 0)
                self.assertIn("OP-Z firmware 0.0.1", text)
                rc, text, _ = run([pkg, "--key-file", kf, "--check"])
                self.assertEqual(rc, 0)
                self.assertFalse(os.path.exists(out))
                self.assertEqual(run([pkg, "--out", out])[0], 1)          # no key
                rc, text, _ = run([pkg, "--key-file", kf, "--out", out])
                self.assertEqual(rc, 0, text)
                with open(os.path.join(out, "test.zip"), "rb") as f:
                    self.assertEqual(f.read(), self.zipped)
                rc, _, err = run([pkg, "--key-file", kf, "--out", out])
                self.assertEqual(rc, 1)
                self.assertIn("--force", err)
                self.assertEqual(run([pkg, "--key-file", kf, "--out", out, "--force"])[0], 0)
            self.assertNotIn(self.key.hex(), text.lower())
            rc, text, _ = run([pkg, "--key-file", kf, "--check", "--allow-unknown"])
            self.assertEqual(rc, 0)
            self.assertNotIn("opz.ldr", text)        # no OS hash to compare for an unknown package


class OutputFolder(unittest.TestCase):
    def test_refuses_the_repository_outside_build(self):
        for d in ("tools/x", "docs", ""):
            with self.subTest(d):
                with self.assertRaisesRegex(opz_zfw.ZfwError, "inside the checkout"):
                    opz_zfw.check_out_dir(os.path.join(REPO, d))

    def test_outside_any_checkout_is_fine(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(opz_zfw.check_out_dir(os.path.join(d, "fw"), repo=d + "-elsewhere"),
                             os.path.realpath(os.path.join(d, "fw")))

    @unittest.skipUnless(shutil.which("git"), "git is not installed")
    def test_build_only_when_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(["git", "init", "-q", d], check=True)
            fw = os.path.join(d, "build", "firmware")
            with self.assertRaisesRegex(opz_zfw.ZfwError, "not ignored"):
                opz_zfw.check_out_dir(fw, repo=d)
            with open(os.path.join(d, ".gitignore"), "w") as f:
                f.write("build/\n")
            self.assertEqual(opz_zfw.check_out_dir(fw, repo=d), os.path.realpath(fw))
            with self.assertRaisesRegex(opz_zfw.ZfwError, "inside the checkout"):
                opz_zfw.check_out_dir(os.path.join(d, "src"), repo=d)


def _te_key():
    """The first key file in keys/ that reads as a key, or None."""
    if not os.path.isdir(KEYS):
        return None
    for n in sorted(os.listdir(KEYS)):
        try:
            return opz_zfw.read_key(os.path.join(KEYS, n))
        except (opz_zfw.ZfwError, OSError):
            continue
    return None


@unittest.skipUnless(os.path.exists(TE_PACKAGE) and os.path.isdir(KEYS),
                     "TE's z_firmware_1_2_45.zfw or keys/ is not in the repository")
class TePackage(unittest.TestCase):
    def test_1_2_45(self):
        with open(TE_PACKAGE, "rb") as f:
            package = f.read()
        entry = opz_zfw.identify(package)
        self.assertIsNotNone(entry, "the package is not the official 1.2.45 file")
        key = _te_key()
        if key is None:
            self.skipTest("no key file in keys/ reads as a key")
        _, zipped = opz_zfw.decrypt(package, key)
        lines = opz_zfw.check_zip(zipped, entry)
        self.assertIn("matches 1.2.45", lines[-1])
        if os.path.exists(TE_ZIP):
            with open(TE_ZIP, "rb") as f:
                self.assertEqual(hashlib.sha256(zipped).digest(), hashlib.sha256(f.read()).digest())


if __name__ == "__main__":
    unittest.main()
