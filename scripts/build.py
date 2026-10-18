"""Build against the exact official OBS 32.0.4 runtime on GitHub Actions."""
import hashlib
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())) / "obs-beauty-sdk"
BASE = "https://github.com/obsproject/obs-studio/releases/download/32.0.4/"


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def download(name, digest):
    path = DEPS / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        request = urllib.request.Request(BASE + name, headers={"User-Agent": "OBS-Beauty"})
        with urllib.request.urlopen(request, timeout=120) as response, path.open("wb") as output:
            shutil.copyfileobj(response, output)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError("Official OBS archive failed SHA-256 verification: " + name)
    return path


DEPS.mkdir(parents=True, exist_ok=True)
source_archive = download("OBS-Studio-32.0.4-Sources.tar.gz",
                          "d9e62ea24305fbfe14c69a6422de1525fe84da232df2cc0d7e4601a840bdbba8")
source = DEPS / "obs-studio-32.0.4-sources"
if not (source / "libobs/obs-module.h").exists():
    run("tar", "-xf", source_archive, "-C", DEPS, "obs-studio-32.0.4-sources/libobs")
if sys.platform == "win32":
    archive = download("OBS-Studio-32.0.4-Windows-x64.zip",
                       "fd367a7f156319426f1a600720c1288ad0011da168dff41e0f94dbb18cc1dc0f")
    runtime = DEPS / "obs-runtime"
    runtime.mkdir(exist_ok=True)
    if not (runtime / "bin/64bit/obs.dll").exists():
        run("tar", "-xf", archive, "-C", runtime)
    generator = ["-G", "Visual Studio 17 2022", "-A", "x64"]
elif sys.platform == "darwin":
    archive = download("OBS-Studio-32.0.4-macOS-Apple.dmg",
                       "7c056644636a794cb18281af98d54b5795ad8b0985e950d93b8c510b645b6298")
    runtime = DEPS / "OBS.app"
    if not runtime.exists():
        mount = DEPS / "obs-mount"
        mount.mkdir(exist_ok=True)
        run("hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mount, archive)
        try:
            run("ditto", mount / "OBS.app", runtime)
        finally:
            run("hdiutil", "detach", mount)
    with (runtime / "Contents/Info.plist").open("rb") as handle:
        if plistlib.load(handle)["CFBundleShortVersionString"] != "32.0.4":
            raise RuntimeError("Unexpected OBS runtime version")
    run("brew", "install", "simde")
    generator = ["-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release"]
    print("Frameworks:", sorted(path.name for path in (runtime / "Contents/Frameworks").iterdir()), flush=True)
else:
    raise RuntimeError("Use a Windows x64 or macOS arm64 GitHub runner")

run("cmake", "-S", ROOT, "-B", ROOT / "build", *generator,
    "-DOBS_ROOT=" + str(runtime), "-DOBS_SOURCE_DIR=" + str(source), "-DBUILD_TESTING=ON")
run("cmake", "--build", ROOT / "build", "--config", "Release", "--parallel", "2")
if os.environ.get("GITHUB_ENV"):
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
        output.write("OBS_ROOT=" + str(runtime) + "\n")
print("OBS runtime:", runtime, flush=True)
