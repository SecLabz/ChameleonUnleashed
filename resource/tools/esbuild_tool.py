"""Fetch the pinned native esbuild executable using only Python's standard library."""
from __future__ import annotations

import base64
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import tarfile
import tempfile
from urllib.request import urlopen

CONFIG = Path(__file__).with_name("esbuild.json")
CACHE = Path(__file__).resolve().parents[2] / ".cache" / "esbuild"


def platform_key() -> str:
    systems = {"Darwin": "darwin", "Linux": "linux", "Windows": "win32"}
    machines = {
        "amd64": "x64", "x86_64": "x64", "aarch64": "arm64",
        "arm64": "arm64", "armv7l": "arm",
    }
    system = systems.get(platform.system(), platform.system().lower())
    machine = machines.get(platform.machine().lower(), platform.machine().lower())
    return f"{system}-{machine}"


def verified_binary(archive: bytes, integrity: str, member: str) -> bytes:
    actual = "sha512-" + base64.b64encode(hashlib.sha512(archive).digest()).decode("ascii")
    if actual != integrity:
        raise RuntimeError("esbuild archive integrity mismatch; refusing to execute it")
    # Extract only the expected regular file, never archive paths or symlinks.
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as package:
        info = package.getmember(member)
        if not info.isfile():
            raise RuntimeError("esbuild archive does not contain a regular executable")
        with package.extractfile(info) as executable:
            return executable.read()


def atomic_write(path: Path, data: bytes, executable: bool = False) -> None:
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary_path = Path(temporary.name)
        try:
            temporary.write(data)
            temporary.close()
            if executable:
                temporary_path.chmod(0o755)
            os.replace(temporary_path, path)
        finally:
            temporary_path.unlink(missing_ok=True)


def ensure_esbuild() -> Path:
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    key = platform_key()
    if key not in config["integrity"]:
        raise RuntimeError(f"Unsupported esbuild host: {key}")
    version = config["version"]
    directory = CACHE / version / key
    archive_path = directory / "package.tgz"
    member = "package/esbuild.exe" if key.startswith("win32-") else "package/bin/esbuild"
    binary = directory / Path(member).name
    if archive_path.is_file():
        archive = archive_path.read_bytes()
    else:
        url = f"https://registry.npmjs.org/@esbuild/{key}/-/{key}-{version}.tgz"
        print(f"Downloading standalone esbuild {version} ({key})", flush=True)
        try:
            with urlopen(url, timeout=60) as response:
                archive = response.read()
        except OSError as error:
            raise RuntimeError(
                f"Cannot download esbuild. Connect once to populate {directory}, "
                "then builds work offline."
            ) from error
    content = verified_binary(archive, config["integrity"][key], member)
    directory.mkdir(parents=True, exist_ok=True)
    if not archive_path.is_file():
        atomic_write(archive_path, archive)
    if not binary.is_file() or binary.read_bytes() != content:
        atomic_write(binary, content, executable=True)
    return binary
