"""Create a reproducible release ZIP from the CMake install tree."""
import stat
import sys
import zipfile
from pathlib import Path

stage, archive = (Path(value).resolve() for value in sys.argv[1:])
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
    for path in sorted(stage.rglob("*")):
        if path.is_symlink():
            raise RuntimeError(f"Unexpected package symlink: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(stage).as_posix()
        executable = relative.endswith(".sh") or "/Contents/MacOS/" in relative
        entry = zipfile.ZipInfo(relative, (2020, 1, 1, 0, 0, 0))
        entry.create_system = 3
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
        output.writestr(entry, path.read_bytes(), compresslevel=9)
print(f"Package: {archive}")
