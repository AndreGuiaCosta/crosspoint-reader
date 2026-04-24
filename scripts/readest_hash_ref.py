#!/usr/bin/env python3
"""
Reference implementation of Readest's book_hash (partial MD5 of raw file bytes)
and meta_hash (MD5 of normalized metadata). Run this on the same EPUB the
device is processing; the 32-hex outputs MUST match the LOG_DBG lines emitted
by ReadestHash::partialMd5 / ReadestHash::metaMd5, or sync will silently fail.

Source algorithms:
  - partialMD5 : apps/readest-app/src/utils/md5.ts:11-30
  - metaHash   : apps/readest-app/src/utils/book.ts:261-323

Usage:
    python3 scripts/readest_hash_ref.py path/to/book.epub
"""

from __future__ import annotations

import hashlib
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


# ---------- partial MD5 ----------

SAMPLE = 1024
STEP = 1024


def partial_md5(path: Path) -> str:
    """Port of `partialMD5` in md5.ts. Samples up to ~12 KiB at logarithmic
    offsets (256, 1024, 4096, ..., 1 GiB) and MD5s the concatenation."""
    md5 = hashlib.md5()
    size = path.stat().st_size
    with path.open("rb") as f:
        for i in range(-1, 11):
            start = STEP >> (2 * -i) if i < 0 else STEP << (2 * i)
            if start >= size:
                break
            end = min(start + SAMPLE, size)
            f.seek(start)
            md5.update(f.read(end - start))
    return md5.hexdigest()


# ---------- metadata extraction ----------

NS = {
    "opf": "http://www.idpf.org/2007/opf",
    "dc": "http://purl.org/dc/elements/1.1/",
    "container": "urn:oasis:names:tc:opendocument:xmlns:container",
}


def _find_opf_path(zf: zipfile.ZipFile) -> str:
    container_xml = zf.read("META-INF/container.xml")
    root = ET.fromstring(container_xml)
    rootfile = root.find(".//container:rootfile", NS)
    if rootfile is None or "full-path" not in rootfile.attrib:
        raise RuntimeError("container.xml has no <rootfile full-path=...>")
    return rootfile.attrib["full-path"]


def _extract_metadata(zf: zipfile.ZipFile) -> tuple[str, list[str], list[tuple[str, str]]]:
    opf_path = _find_opf_path(zf)
    opf = ET.fromstring(zf.read(opf_path))
    # Element truthiness is based on child count, so "or" can misbehave when
    # the first match returns an empty element. Use explicit None checks.
    metadata = opf.find(".//opf:metadata", NS)
    if metadata is None:
        metadata = opf.find(".//{*}metadata")
    if metadata is None:
        return "", [], []

    title = ""
    authors: list[str] = []
    identifiers: list[tuple[str, str]] = []

    for child in metadata:
        # Strip namespace from the tag, e.g. "{http://...}title" -> "title".
        tag = re.sub(r"^\{[^}]+\}", "", child.tag)
        text = (child.text or "")
        if tag == "title" and not title:
            title = text
        elif tag == "creator":
            trimmed = text.strip()
            if trimmed:
                authors.append(trimmed)
        elif tag == "identifier":
            # opf:scheme attribute may be qualified or not
            scheme = ""
            for attr_name, attr_val in child.attrib.items():
                if re.sub(r"^\{[^}]+\}", "", attr_name) == "scheme":
                    scheme = attr_val.lower()
                    break
            identifiers.append((scheme, text))

    return title, authors, identifiers


# ---------- meta hash ----------

def _normalize_identifier(value: str) -> str:
    """Replicates book.ts:261-274. `urn:…` uses last `:`; otherwise first `:`."""
    if "urn:" in value:
        last_colon = value.rfind(":")
        if last_colon != -1:
            return value[last_colon + 1 :]
    first_colon = value.find(":")
    if first_colon != -1:
        return value[first_colon + 1 :]
    return value


def _pick_preferred(identifiers: list[tuple[str, str]]) -> tuple[str, str] | None:
    """Priority: uuid > calibre > isbn (substring, case-insensitive)."""
    for preferred in ("uuid", "calibre", "isbn"):
        for scheme, value in identifiers:
            if preferred in scheme:
                return (scheme, value)
    return None


def meta_hash(path: Path) -> tuple[str, str]:
    """Returns (meta_hash_hex, hash_source_string) for the given EPUB."""
    with zipfile.ZipFile(path) as zf:
        title, authors, identifiers = _extract_metadata(zf)

    preferred = _pick_preferred(identifiers)
    if preferred is not None:
        identifier_part = _normalize_identifier(preferred[1])
    else:
        identifier_part = ",".join(_normalize_identifier(v) for _, v in identifiers)

    hash_source = f"{title}|{','.join(authors)}|{identifier_part}"
    # NFC normalization — Readest's JS String.normalize("NFC") equivalent.
    normalized = unicodedata.normalize("NFC", hash_source)

    return hashlib.md5(normalized.encode("utf-8")).hexdigest(), hash_source


# ---------- CLI ----------

def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"not a file: {path}", file=sys.stderr)
        return 1

    book = partial_md5(path)
    meta, source = meta_hash(path)
    print(f"book_hash: {book}")
    print(f"meta_hash: {meta}")
    print(f"  source : {source!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
