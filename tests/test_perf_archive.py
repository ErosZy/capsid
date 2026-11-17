"""Transport controls only; a successful unpack is never performance acceptance."""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
import perf_archive as packed
from perf_evidence import EvidenceError

PREFIX = "bench/results/test-session-cold"
BODY = b"original sealed evidence bytes\n"


class PackedArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="capsid-packed-control-")
        self.root = Path(self.temp.name)
        (self.root / "perf-site/archives").mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self, members=None, files=1, total=None, multipart=False, trailer=b""):
        members = members or [(PREFIX + "/sample.json", BODY, tarfile.REGTYPE)]
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w") as archive:
            for name, content, kind in members:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = len(content) if kind == tarfile.REGTYPE else 0
                if kind == tarfile.SYMTYPE:
                    member.linkname = "../../escape"
                archive.addfile(member, io.BytesIO(content) if member.size else None)
        data = gzip.compress(raw.getvalue() + trailer, mtime=0)
        chunks = [data] if not multipart else [data[:17], data[17:]]
        parts = []
        for number, content in enumerate(chunks):
            name = f"perf-site/archives/test-{number}.tar.gz"
            (self.root / name).write_bytes(content)
            parts.append({"path": name, "sha256": hashlib.sha256(content).hexdigest(),
                          "bytes": len(content)})
        bundle = {"lane": "cold-start", "prefix": PREFIX, "files": files,
                  "uncompressedBytes": total if total is not None else
                  sum(len(body) for _, body, kind in members if kind == tarfile.REGTYPE) or 1,
                  "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "parts": parts}
        index = {"schema": 1, "contract": "capsid-perf-packed-archives-v1", "bundles": [bundle]}
        self.write_index(index)
        return index

    def write_index(self, value):
        (self.root / "perf-site/archive-index.json").write_text(json.dumps(value), encoding="utf-8")

    def test_restores_multipart_bytes_without_changing_input(self):
        index = self.fixture(multipart=True)
        source = [(self.root / p["path"]).read_bytes() for p in index["bundles"][0]["parts"]]
        target = self.root / "restored"
        report = packed.materialize(self.root, target, [PREFIX])
        self.assertEqual((target / PREFIX / "sample.json").read_bytes(), BODY)
        self.assertEqual(report["files"], {PREFIX + "/sample.json": hashlib.sha256(BODY).hexdigest()})
        self.assertEqual(source, [(self.root / p["path"]).read_bytes()
                                  for p in index["bundles"][0]["parts"]])
        with self.assertRaisesRegex(EvidenceError, "must be new"):
            packed.materialize(self.root, target)

    def test_restores_bounded_pax_long_names(self):
        name = PREFIX + "/" + "long-directory-" * 10 + "/sample.json"
        self.fixture([(name, BODY, tarfile.REGTYPE)])
        target = self.root / "long-path"
        packed.materialize(self.root, target)
        self.assertEqual((target / name).read_bytes(), BODY)

    def test_rejects_metadata_allocations_before_processing(self):
        for number, size in enumerate([8193, 2 ** 30]):
            index = self.fixture()
            header = tarfile.TarInfo("pax-metadata")
            header.type, header.size = tarfile.XHDTYPE, size
            data = gzip.compress(header.tobuf(format=tarfile.USTAR_FORMAT) + b"\0" * 1024, mtime=0)
            part = index["bundles"][0]["parts"][0]
            (self.root / part["path"]).write_bytes(data)
            part["bytes"], part["sha256"] = len(data), hashlib.sha256(data).hexdigest()
            index["bundles"][0].update(bytes=len(data), sha256=part["sha256"])
            self.write_index(index)
            with self.subTest(size=size), self.assertRaisesRegex(EvidenceError, "metadata header"):
                packed.materialize(self.root, self.root / ("metadata-" + str(number)))
            self.assertFalse((self.root / ("metadata-" + str(number)) / PREFIX).exists())

    def test_rejects_chained_pax_metadata_before_recursion(self):
        index = self.fixture()
        part = index["bundles"][0]["parts"][0]
        original = gzip.decompress((self.root / part["path"]).read_bytes())
        header = tarfile.TarInfo("pax-metadata")
        header.type = tarfile.XHDTYPE
        block = header.tobuf(format=tarfile.USTAR_FORMAT)
        data = gzip.compress(block + block + original, mtime=0)
        (self.root / part["path"]).write_bytes(data)
        part["bytes"], part["sha256"] = len(data), hashlib.sha256(data).hexdigest()
        index["bundles"][0].update(bytes=len(data), sha256=part["sha256"])
        self.write_index(index)
        with self.assertRaisesRegex(EvidenceError, "metadata header"):
            packed.materialize(self.root, self.root / "chained-pax")

    def test_rejects_coherently_rehashed_bad_gzip_crc(self):
        index = self.fixture()
        part = index["bundles"][0]["parts"][0]
        data = bytearray((self.root / part["path"]).read_bytes())
        data[-8] ^= 1
        (self.root / part["path"]).write_bytes(data)
        part["sha256"] = hashlib.sha256(data).hexdigest()
        index["bundles"][0]["sha256"] = part["sha256"]
        self.write_index(index)
        with self.assertRaisesRegex(EvidenceError, "invalid packed archive stream"):
            packed.materialize(self.root, self.root / "bad-crc")

    def test_rejects_changed_transport_before_unpacking(self):
        index = self.fixture()
        path = self.root / index["bundles"][0]["parts"][0]["path"]
        data = bytearray(path.read_bytes()); data[12] ^= 1; path.write_bytes(data)
        with self.assertRaisesRegex(EvidenceError, "part digest mismatch"):
            packed.materialize(self.root, self.root / "bad")
        self.assertFalse((self.root / "bad" / PREFIX).exists())

    def test_rejects_unsafe_members_and_links(self):
        members = [PREFIX + "/../../escape", "/absolute", "C:/drive", PREFIX + "/name:ads",
                   PREFIX + "/../escape", PREFIX + "/back\\slash"]
        for number, name in enumerate(members):
            self.fixture([(name, BODY, tarfile.REGTYPE)])
            with self.subTest(name=name), self.assertRaises(EvidenceError):
                packed.materialize(self.root, self.root / ("unsafe-" + str(number)))
        for number, kind in enumerate([tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE]):
            self.fixture([(PREFIX + "/special", b"", kind)])
            with self.subTest(kind=kind), self.assertRaisesRegex(EvidenceError, "non-regular"):
                packed.materialize(self.root, self.root / ("link-" + str(number)))
        self.assertFalse((self.root / "escape").exists())

    def test_rejects_duplicate_case_alias_and_wrong_prefix(self):
        for number, names in enumerate([(PREFIX + "/one", PREFIX + "/one"),
                                        (PREFIX + "/one", PREFIX + "/ONE"),
                                        (PREFIX + "/one", "bench/results/other/two")]):
            self.fixture([(n, BODY, tarfile.REGTYPE) for n in names], files=2)
            with self.subTest(names=names), self.assertRaises(EvidenceError):
                packed.materialize(self.root, self.root / ("alias-" + str(number)))

    def test_rejects_declared_file_and_byte_count_lies(self):
        for number, (files, total) in enumerate([(2, len(BODY)), (1, len(BODY) + 1),
                                               (1, len(BODY) - 1)]):
            self.fixture(files=files, total=total)
            with self.subTest(files=files, total=total), self.assertRaises(EvidenceError):
                packed.materialize(self.root, self.root / ("bounds-" + str(number)))

    def test_rejects_extra_files_and_decompression_bomb_padding(self):
        self.fixture([(PREFIX + "/one", BODY, tarfile.REGTYPE),
                      (PREFIX + "/two", BODY, tarfile.REGTYPE)], files=1)
        with self.assertRaisesRegex(EvidenceError, "declared bounds"):
            packed.materialize(self.root, self.root / "extra")
        self.fixture(trailer=b"\0" * (2 * 1024 * 1024))
        with self.assertRaisesRegex(EvidenceError, "decompressed transport exceeds bound"):
            packed.materialize(self.root, self.root / "bomb")

    def test_rejects_catalog_duplicates_misplaced_parts_and_unexpected_scope(self):
        index = self.fixture()
        with self.assertRaisesRegex(EvidenceError, "match verified points"):
            packed.materialize(self.root, self.root / "scope", ["bench/results/another"])
        self.assertFalse((self.root / "scope").exists())
        index["bundles"][0]["parts"][0]["path"] = "README.md"
        self.write_index(index)
        with self.assertRaisesRegex(EvidenceError, "misplaced"):
            packed.materialize(self.root, self.root / "misplaced")
        (self.root / "perf-site/archive-index.json").write_text('{"schema":1,"schema":1}', encoding="utf-8")
        with self.assertRaises(EvidenceError):
            packed.materialize(self.root, self.root / "duplicate-json")


if __name__ == "__main__":
    unittest.main()
