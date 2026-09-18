#!/usr/bin/env python3
"""Build a deterministic .liberaplugin ZIP from a package directory."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shutil
import stat
import sys
import tempfile
import zipfile
import zlib


ID_PATTERN = re.compile(r"^[a-z0-9]+(?:[.-][a-z0-9]+)*$")
VERSION_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)
CONTROLLER_TYPE_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
WINDOWS_RESERVED = {
    "con", "prn", "aux", "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}
MAXIMUM_PACKAGE_BYTES = 1024 * 1024 * 1024
MAXIMUM_EXTRACTED_BYTES = 1024 * 1024 * 1024
MAXIMUM_FILE_BYTES = 512 * 1024 * 1024
MAXIMUM_MANIFEST_BYTES = 1024 * 1024
MAXIMUM_ARCHIVE_ENTRIES = 512
MAXIMUM_COMPRESSION_RATIO = 1000
PRESERVED_ARCHIVE_NAME = "archive.liberaplugin"


def portable_path(value: str) -> pathlib.PurePosixPath:
    if not value or len(value) > 1024 or "\\" in value or value.startswith("/"):
        raise ValueError(f"unsafe package path: {value!r}")
    raw_parts = value.split("/")
    if any(part in {"", ".", ".."} for part in raw_parts):
        raise ValueError(f"unsafe package path: {value!r}")
    path = pathlib.PurePosixPath(value)
    for part in path.parts:
        if any(ord(char) < 32 or ord(char) > 126 for char in part):
            raise ValueError(f"package paths must use printable ASCII: {value!r}")
        if any(char in '<>:"|?*' for char in part) or part.endswith((".", " ")):
            raise ValueError(f"path is not portable to Windows: {value!r}")
        if part.split(".", 1)[0].casefold() in WINDOWS_RESERVED:
            raise ValueError(f"reserved Windows filename in path: {value!r}")
    return path


def required_string(manifest: dict, key: str) -> str:
    value = manifest.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"manifest requires non-empty string {key!r}")
    return value


def utf8_size(value: str) -> int:
    return len(value.encode("utf-8"))


def valid_single_line_text(value: str, maximum_bytes: int) -> bool:
    return (0 < utf8_size(value) <= maximum_bytes and
            all(ord(char) >= 32 and ord(char) != 127 for char in value))


def valid_description(value: str) -> bool:
    return (utf8_size(value) <= 4096 and
            all((ord(char) >= 32 or char in "\n\r\t") and ord(char) != 127
                for char in value))


def validate_manifest(manifest: dict, files: set[str]) -> None:
    if type(manifest.get("schemaVersion")) is not int or manifest["schemaVersion"] != 1:
        raise ValueError("schemaVersion must be 1")
    plugin_id = required_string(manifest, "id")
    version = required_string(manifest, "version")
    name = required_string(manifest, "name")
    vendor = required_string(manifest, "vendor")
    controller_type = required_string(manifest, "controllerType")
    if utf8_size(plugin_id) > 128 or not ID_PATTERN.fullmatch(plugin_id):
        raise ValueError("id must be a lowercase dotted or hyphenated identifier")
    if plugin_id == "libera.builtin" or plugin_id.startswith("libera.builtin."):
        raise ValueError("id uses the reserved libera.builtin namespace")
    if utf8_size(version) > 64 or not VERSION_PATTERN.fullmatch(version):
        raise ValueError("version must use semantic versioning")
    if (not valid_single_line_text(name, 256) or
            not valid_single_line_text(vendor, 256)):
        raise ValueError("name and vendor must be single-line text of at most 256 bytes")
    if (len(controller_type) > 128 or
            not CONTROLLER_TYPE_PATTERN.fullmatch(controller_type)):
        raise ValueError("controllerType contains invalid characters")
    description = manifest.get("description", "")
    if not isinstance(description, str) or not valid_description(description):
        raise ValueError("description contains invalid control characters or exceeds 4096 bytes")
    libera = manifest.get("libera")
    if (not isinstance(libera, dict) or
            type(libera.get("abiVersion")) is not int or
            libera["abiVersion"] != 1):
        raise ValueError("libera.abiVersion must be 1")
    entrypoints = manifest.get("entrypoints")
    if not isinstance(entrypoints, list) or not entrypoints:
        raise ValueError("entrypoints must be a non-empty array")
    targets: set[tuple[str, str]] = set()
    for entrypoint in entrypoints:
        if not isinstance(entrypoint, dict):
            raise ValueError("each entrypoint must be an object")
        target = (required_string(entrypoint, "os"),
                  required_string(entrypoint, "arch"))
        if target[0] not in {"windows", "macos", "linux"} or target[1] not in {
            "arm64", "x86_64", "x86", "universal"
        }:
            raise ValueError(f"unsupported entrypoint target: {target}")
        path = str(portable_path(required_string(entrypoint, "path")))
        expected_extension = {
            "windows": ".dll",
            "macos": ".dylib",
            "linux": ".so",
        }[target[0]]
        if pathlib.PurePosixPath(path).suffix.casefold() != expected_extension:
            raise ValueError(f"entrypoint extension does not match {target[0]}: {path}")
        if target in targets:
            raise ValueError(f"duplicate entrypoint target: {target}")
        targets.add(target)
        if path not in files:
            raise ValueError(f"entrypoint is missing: {path}")
    documents = manifest.get("documents", {})
    if not isinstance(documents, dict):
        raise ValueError("documents must be an object")
    for key in ("readme", "license"):
        if key in documents:
            path = str(portable_path(required_string(documents, key)))
            if path not in files:
                raise ValueError(f"document is missing: {path}")


def collect_files(source: pathlib.Path,
                  output: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    result: list[tuple[str, pathlib.Path]] = []
    seen: set[str] = set()
    total_size = 0
    for candidate in source.rglob("*"):
        if candidate == output:
            continue
        if candidate.is_symlink():
            raise ValueError(f"symlinks are not allowed: {candidate}")
        mode = candidate.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f"special files are not allowed: {candidate}")
        relative = candidate.relative_to(source).as_posix()
        portable_path(relative)
        if relative.casefold() == PRESERVED_ARCHIVE_NAME:
            raise ValueError(f"reserved installer path: {relative}")
        folded = relative.casefold()
        if folded in seen:
            raise ValueError(f"case-insensitive path collision: {relative}")
        size = candidate.stat().st_size
        if size > MAXIMUM_FILE_BYTES:
            raise ValueError(f"file exceeds the package limit: {relative}")
        total_size += size
        if total_size > MAXIMUM_EXTRACTED_BYTES:
            raise ValueError("package exceeds the allowed expanded size")
        seen.add(folded)
        result.append((relative, candidate))
        if len(result) > MAXIMUM_ARCHIVE_ENTRIES:
            raise ValueError("package contains too many files")
    result.sort(key=lambda item: item[0])
    file_names = {name.casefold() for name, _ in result}
    for name, _ in result:
        parts = name.casefold().split("/")
        for index in range(1, len(parts)):
            if "/".join(parts[:index]) in file_names:
                raise ValueError(f"file also acts as a directory: {name}")
    return result


def compression_type(path: pathlib.Path) -> int:
    size = path.stat().st_size
    if size <= 1024 * 1024:
        return zipfile.ZIP_DEFLATED

    compressor = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    compressed_size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            compressed_size += len(compressor.compress(chunk))
    compressed_size += len(compressor.flush())
    if (compressed_size == 0 or
            size // compressed_size > MAXIMUM_COMPRESSION_RATIO):
        return zipfile.ZIP_STORED
    return zipfile.ZIP_DEFLATED


def build(source: pathlib.Path, output: pathlib.Path) -> None:
    source = source.resolve()
    output = output.resolve()
    manifest_path = source / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError("source directory must contain manifest.json")
    files = collect_files(source, output)
    names = {name for name, _ in files}
    if manifest_path.stat().st_size > MAXIMUM_MANIFEST_BYTES:
        raise ValueError("manifest.json exceeds the package limit")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict):
        raise ValueError("manifest root must be an object")
    validate_manifest(manifest, names)
    if output.suffix.casefold() != ".liberaplugin":
        raise ValueError("output must end in .liberaplugin")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_file = tempfile.NamedTemporaryFile(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False)
    temporary = pathlib.Path(temporary_file.name)
    temporary_file.close()
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED,
                             compresslevel=9) as archive:
            for name, path in files:
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = compression_type(path)
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                with path.open("rb") as source_file, archive.open(info, "w") as target:
                    shutil.copyfileobj(source_file, target, length=1024 * 1024)
        if temporary.stat().st_size > MAXIMUM_PACKAGE_BYTES:
            raise ValueError("compressed package exceeds the allowed size")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path,
                        help="directory containing manifest.json and package files")
    parser.add_argument("output", type=pathlib.Path,
                        help="output path ending in .liberaplugin")
    args = parser.parse_args()
    try:
        build(args.source, args.output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
