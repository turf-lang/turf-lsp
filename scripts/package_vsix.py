#!/usr/bin/env python3
"""Assemble and package the VS Code extension for ONE platform, and the same
server as a standalone archive for other editors.

The extension carries its own language server and standard library, so that
installing it is all a user does. That only works if the server binary needs
nothing the user's machine may lack. This script therefore refuses a server that
links against anything but the operating system's own libraries (no LLVM, no C++
runtime to be installed): a VSIX that fails on first use is worse than none.

    scripts/package_vsix.py --target linux-x64 \\
        --server build/turf-lsp --turf ../turf-lang/build/turf --stdlib ../turf-lang

--stdlib is a Turf installation or checkout: its lib/std is bundled.
--turf is the `turf` command-line tool (the language server asks it for a
project's package graph); it is Go and links statically.
--allow-dynamic packages a server that links shared libraries anyway. That is
for trying the extension on the machine that built it, never for release.

Targets are VS Code's: linux-x64, linux-arm64, win32-x64, win32-arm64,
darwin-x64, darwin-arm64.
"""

import argparse
import json
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXT = ROOT / "editors" / "vscode"

# The operating system's own libraries, which every machine of the platform has.
SYSTEM_LIBS = {
    # (the loader is reported with its directory: /lib64/ld-linux-x86-64.so.2)
    "linux": re.compile(r"^(?:.*/)?(linux-vdso\.so|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libutil\.so|ld-linux)"),
    "darwin": re.compile(r"^/(usr/lib/(libSystem|libc\+\+|libc\+\+abi|libz|libncurses|libobjc|libresolv)|System/Library/)"),
    "win32": re.compile(r"^(kernel32|ntdll|ucrtbase|msvcrt|advapi32|user32|shell32|ole32|oleaut32|ws2_32|bcrypt|crypt32|version|shlwapi|api-ms-win-)", re.I),
}


def host_target():
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    system = {"Linux": "linux", "Darwin": "darwin", "Windows": "win32"}[platform.system()]
    return "%s-%s" % (system, arch)


def dependencies(binary, os_name):
    """The shared libraries a binary needs, as the platform's own tool reports them."""
    if os_name == "linux":
        out = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
        if out.returncode != 0 and "not a dynamic executable" in (out.stdout + out.stderr):
            return []  # statically linked
        return [line.split()[0] for line in out.stdout.splitlines() if line.strip()]
    if os_name == "darwin":
        out = subprocess.run(["otool", "-L", str(binary)], capture_output=True, text=True)
        return [line.strip().split(" ")[0] for line in out.stdout.splitlines()[1:] if line.strip()]
    tool = shutil.which("llvm-objdump") or shutil.which("objdump")
    if not tool:
        print("warning: no objdump to inspect %s's DLL dependencies; not checked" % binary.name)
        return []
    out = subprocess.run([tool, "-p", str(binary)], capture_output=True, text=True)
    return re.findall(r"DLL Name: (\S+)", out.stdout)


def foreign_dependencies(binary, os_name):
    allowed = SYSTEM_LIBS[os_name]
    return [d for d in dependencies(binary, os_name) if not allowed.match(d)]


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default=host_target())
    ap.add_argument("--server", required=True, type=pathlib.Path)
    ap.add_argument("--turf", required=True, type=pathlib.Path)
    ap.add_argument("--stdlib", required=True, type=pathlib.Path)
    ap.add_argument("--out", default=ROOT / "dist", type=pathlib.Path)
    ap.add_argument("--allow-dynamic", action="store_true")
    ap.add_argument("--check-only", action="store_true", help="only check the server's dependencies")
    args = ap.parse_args()

    os_name = args.target.split("-")[0]
    if os_name not in SYSTEM_LIBS:
        sys.exit("unknown target %r" % args.target)
    exe = ".exe" if os_name == "win32" else ""
    for what, p in (("server", args.server), ("turf", args.turf)):
        if not p.is_file():
            sys.exit("--%s: %s does not exist" % (what, p))
    std = args.stdlib / "lib" / "std"
    if not (std / "core").is_dir():
        sys.exit("--stdlib: %s has no lib/std/core" % args.stdlib)

    # 1. The server must stand on its own.
    foreign = foreign_dependencies(args.server, os_name)
    if foreign:
        message = "the server links against libraries a user's machine may not have:\n  " + "\n  ".join(foreign)
        if not args.allow_dynamic:
            sys.exit("refusing to package: " + message +
                     "\n(--allow-dynamic packages it anyway, for trying it on this machine only)")
        print("WARNING: " + message + "\n         this package will not work elsewhere.")
    else:
        print("server dependencies: only the operating system's own libraries")
    if args.check_only:
        return

    # 2. Assemble server/ inside the extension: bin/ and lib/std, the layout the
    #    server finds its standard library from (<root>/bin/.. = <root>).
    server_dir = EXT / "server"
    if server_dir.exists():
        shutil.rmtree(server_dir)
    (server_dir / "bin").mkdir(parents=True)
    shutil.copy2(args.server, server_dir / "bin" / ("turf-lsp" + exe))
    shutil.copy2(args.turf, server_dir / "bin" / ("turf" + exe))
    shutil.copytree(std, server_dir / "lib" / "std")
    for name in ("LICENSE", "LICENSE.md", "LICENSE.txt"):
        if (args.stdlib / name).is_file():
            shutil.copy2(args.stdlib / name, EXT / "LICENSE")
            break

    # 3. Build and package.
    run(["npm", "ci"], cwd=EXT, shell=(os_name == "win32"))
    run(["npm", "run", "compile"], cwd=EXT, shell=(os_name == "win32"))
    version = json.loads((EXT / "package.json").read_text())["version"]
    args.out.mkdir(parents=True, exist_ok=True)
    vsix = (args.out / ("turf-%s-%s.vsix" % (args.target, version))).resolve()
    run(["npx", "--yes", "@vscode/vsce", "package", "--target", args.target, "--out", vsix],
        cwd=EXT, shell=(os_name == "win32"))

    # 4. Look inside: a package that lacks its server is the failure this exists to prevent.
    with zipfile.ZipFile(vsix) as z:
        names = set(z.namelist())
    needed = ["extension/out/extension.js", "extension/package.json",
              "extension/server/bin/turf-lsp" + exe, "extension/server/bin/turf" + exe,
              "extension/server/lib/std/core/int.tr", "extension/node_modules/vscode-languageclient/package.json",
              "extension/syntaxes/turf.tmLanguage.json"]
    missing = [n for n in needed if n not in names]
    if missing:
        sys.exit("the package is missing: " + ", ".join(missing))
    print("packaged %s (%d bytes, %d files)" % (vsix, vsix.stat().st_size, len(names)))

    # 5. The same server/ for every other editor (Neovim, Emacs, Helix, ...; Mason
    #    installs it from the release): turf-lsp-<target>-<version>/{bin,lib/std}.
    top = "turf-lsp-%s-%s" % (args.target, version)
    if os_name == "win32":
        archive = (args.out / (top + ".zip")).resolve()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted(server_dir.rglob("*")):
                z.write(p, pathlib.PurePosixPath(top, p.relative_to(server_dir).as_posix()))
    else:
        archive = (args.out / (top + ".tar.gz")).resolve()
        with tarfile.open(archive, "w:gz") as t:
            t.add(server_dir, arcname=top)
    print("server archive %s (%d bytes)" % (archive, archive.stat().st_size))


if __name__ == "__main__":
    main()
