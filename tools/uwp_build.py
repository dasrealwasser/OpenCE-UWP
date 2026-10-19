#!/usr/bin/env python3
"""Ninja rules and packaging helpers for the Universal Windows Platform (UWP)
and Xbox Dev Mode build (``ninja uwp``).

The UWP port (port/uwp/README.md) runs the game as 32-bit-pointer ILP32
x86-64 code (``x86_64-linux-gnux32``) inside a 64-bit UWP host executable
(``build/uwp/halo_uwp.exe``) backed by ``SDL3-uwp`` and ``mesa-uwp`` (OpenGL
ES 3.2 over Direct3D 12), and packages everything into a self-contained,
signed ``build/uwp/halo-uwp.msix`` ready for Xbox Dev Mode and Windows 10/11.
"""

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional

if __name__ != "__main__":
    from .android_build import (EXPAT_DIR, EXPAT_SOURCES, KCP_DIR, MBEDTLS_DIR, MONOCYPHER_DIR, MUSL_DIRECTORIES,
                                MUSL_EXCLUDE, MUSL_FILES, MUSL_THREAD_PREFIXES, MUSL_URL, MUSL_VERSION, TOML_DIR,
                                VARIADIC_PROTOTYPE_FILES, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES)
    from .embed_assets import hud_assets_build, hud_configure_inputs
    from .linux_build import (LINUX_PROFILE, MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, STB_VORBIS_SOURCE,
                              WINDOWS_PROFILE, XDK_INCLUDE, compile_launcher, configuration_defines,
                              game_defines_and_includes, game_sources, miniupnpc_sources, musl_math_sources,
                              opus_cflags, opus_sources, pgo_mode, pgo_profile, profile_use_flags, updater_defines,
                              xdk_headers)
    from .ninja_syntax import Writer

ROOT = Path(__file__).resolve().parent.parent
PORT_UWP_DIR = Path("port/uwp")
PORT_ANDROID_DIR = Path("port/android")
PORT_WINDOWS_DIR = Path("port/windows")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/uwp")
THIRD_PARTY = BUILD / "third_party"

MUSL_VERSION_STR = "1.2.5"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION_STR}"
MUSL_ARCHIVE_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION_STR}.tar.gz"

SDL_UWP_VERSION = "3.4.16"
SDL_UWP_URL = f"https://github.com/XboxEmulationHub/SDL3-uwp/releases/download/release-uwp-{SDL_UWP_VERSION}/SDL-UWP.zip"
SDL_UWP_DIR = THIRD_PARTY / f"SDL3-uwp-{SDL_UWP_VERSION}"

MESA_UWP_VERSION = "26.2.2"
MESA_UWP_URL = f"https://github.com/XboxEmulationHub/mesa-uwp/releases/download/{MESA_UWP_VERSION}/{MESA_UWP_VERSION}.zip"
MESA_UWP_DIR = THIRD_PARTY / f"mesa-uwp-{MESA_UWP_VERSION}"

# Guest ABI flags: 64-bit x86-64 instructions with 32-bit pointers and longs
# (ILP32 x32), 64-bit long double, 16-bit wchar_t, 8-byte aligned doubles,
# and no PIC so absolute 32-bit addresses link cleanly at 0x88000000.
GUEST_ABI_FLAGS = [
    "--target=x86_64-linux-gnux32",
    "-march=x86-64",
    "-mlong-double-64",
    "-DHALO_ARM64_GUEST=1",
    "-DHALO_GLES=1",
    "-DHALO_ANDROID=1",
    "-DHALO_UWP=1",
    "-nostdinc",
    "-fshort-wchar",
    "-malign-double",
    "-fno-pic",
    "-fno-pie",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-ffp-contract=off",
    "-O2",
]

GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

HOST_CFLAGS = [
    "--target=x86_64-pc-windows-msvc",
    "-std=gnu11",
    "-O2",
    "-g",
    "-gcodeview",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-D_CRT_NONSTDC_NO_WARNINGS",
    "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
    "-Wall",
    "-Wno-unused-function",
]

HOST_LIBS = [
    "-lws2_32",
    "-liphlpapi",
    "-lbcrypt",
    "-lwinhttp",
    "-lsynchronization",
    "-luser32",
    "-lkernel32",
]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _llvm_tool(cc: str, tool_name: str) -> str:
    path = Path(cc)
    if path.parent != Path("."):
        candidate = path.with_name(tool_name + (".exe" if os.name == "nt" and not tool_name.endswith(".exe") else ""))
        if candidate.is_file():
            return _quote(candidate)
    if os.name == "nt":
        default_llvm = Path("C:/Program Files/LLVM/bin") / f"{tool_name}.exe"
        if default_llvm.is_file():
            return _quote(default_llvm)
    return tool_name


def fetch_uwp_third_party() -> None:
    """Downloads musl, SDL3-uwp, and mesa-uwp once into build/uwp/third_party."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not (MUSL_DIR / "include" / "alltypes.h.in").is_file():
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION_STR}.tar.gz"
        print(f"Downloading {MUSL_ARCHIVE_URL}")
        with urllib.request.urlopen(MUSL_ARCHIVE_URL) as response, open(archive, "wb") as f:
            shutil.copyfileobj(response, f)
        with tarfile.open(archive, "r:gz") as tar:
            tar.extractall(THIRD_PARTY)
        archive.unlink()

    if not (SDL_UWP_DIR / "lib" / "x64" / "Release" / "SDL3.lib").is_file():
        archive = THIRD_PARTY / f"SDL-UWP-{SDL_UWP_VERSION}.zip"
        print(f"Downloading {SDL_UWP_URL}")
        with urllib.request.urlopen(SDL_UWP_URL) as response, open(archive, "wb") as f:
            shutil.copyfileobj(response, f)
        SDL_UWP_DIR.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(SDL_UWP_DIR)
        archive.unlink()

    if not (MESA_UWP_DIR / "x64" / "libGLESv2.dll").is_file():
        archive = THIRD_PARTY / f"mesa-uwp-{MESA_UWP_VERSION}.zip"
        print(f"Downloading {MESA_UWP_URL}")
        with urllib.request.urlopen(MESA_UWP_URL) as response, open(archive, "wb") as f:
            shutil.copyfileobj(response, f)
        MESA_UWP_DIR.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(MESA_UWP_DIR)
        archive.unlink()


def _musl_sources() -> List[Path]:
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def uwp_configure_inputs() -> List[Path]:
    return [
        Path(__file__),
        Path("tools/uwp_gl_stubs.py"),
        Path("tools/uwp_imports.py"),
        PORT_UWP_DIR / "guest" / "guest.ld",
        PORT_UWP_DIR / "host",
        PORT_UWP_DIR / "include" / "GLES3" / "gl32.h",
        PORT_UWP_DIR / "include" / "GLES2" / "gl2ext.h",
        PORT_UWP_DIR / "package" / "Package.appxmanifest.in",
        PORT_UWP_DIR / "package" / "generate_assets.py",
        PORT_ANDROID_DIR / "guest" / "runtime",
        LINUX_DIR / "src",
        *hud_configure_inputs(),
    ]


def generate_uwp_build(n: "Writer", sln: Any) -> None:
    if sys.platform != "win32":
        return
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_UWP_DIR / "host").is_dir():
        return
    try:
        fetch_uwp_third_party()
    except OSError as error:
        print(f"UWP build disabled: cannot fetch musl/SDL3-uwp/mesa-uwp ({error})", file=sys.stderr)
        return

    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    cc = getattr(sln, "windows_cc", None) or "clang"
    llvm_ar = _llvm_tool(cc, "llvm-ar")
    ld_lld = _llvm_tool(cc, "ld.lld")

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = PORT_UWP_DIR / "include"
    arch = PORT_ANDROID_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = BUILD / "halo_msvc_semantics.h"
    platform_semantics_header = BUILD / "platform_msvc_semantics.h"
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    host_exe = BUILD / "halo_uwp.exe"
    msix_out = BUILD / "halo-uwp.msix"
    cer_out = BUILD / "halo-uwp.cer"
    appx_dir = BUILD / "appx"
    python = "$python"

    n.comment("UWP / Xbox Dev Mode build (ninja uwp); see port/uwp/README.md")
    n.variable("uwp_cc", cc)
    n.variable("uwp_ar", llvm_ar)
    n.variable("uwp_ld", ld_lld)

    # ---------- MSVC semantics headers for the guest
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.rule(
        name="uwp_msvc_semantics",
        command=f"{python} tools/linux_msvc_semantics.py --output $out $scan",
        description="UWP MSVC SEMANTICS $out",
        restat=True,
    )
    n.build(
        outputs=semantics_header,
        rule="uwp_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers(), *game_headers],
        variables={"scan": f"--all-inlines --tags source --inlines source --inlines {XDK_INCLUDE}"},
    )
    n.build(
        outputs=platform_semantics_header,
        rule="uwp_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers()],
        variables={"scan": f"--inlines {XDK_INCLUDE}"},
    )

    # ---------- generated musl headers (bits/alltypes.h, bits/syscall.h, version.h)
    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="uwp_musl_headers",
        command=f"{python} tools/uwp_build.py --gen-musl-headers {MUSL_DIR} {arch} {alltypes} {syscall_h} {version_h}",
        description="UWP MUSL HEADERS",
    )
    n.build(
        outputs=[alltypes, syscall_h, version_h],
        rule="uwp_musl_headers",
        inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in", arch / "bits" / "syscall.h.in"],
        implicit=[Path("tools/uwp_build.py")],
    )

    # ---------- generated GL, POSIX, and host import stubs
    guest_gl_c = gen_dir / "guest_gl.c"
    host_gl_table_c = BUILD / "host" / "host_gl_table.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="uwp_gl_stubs",
        command=(f"{python} tools/uwp_gl_stubs.py {LINUX_DIR}/src/gl.h {gl_include}/GLES3/gl32.h "
                 f"{gl_include}/GLES2/gl2ext.h {guest_gl_c} {host_gl_table_c} {gl_imports}"),
        description="UWP GL STUBS",
    )
    n.build(
        outputs=[guest_gl_c, host_gl_table_c, gl_imports],
        rule="uwp_gl_stubs",
        implicit=[
            Path("tools/uwp_gl_stubs.py"),
            LINUX_DIR / "src" / "gl.h",
            gl_include / "GLES3" / "gl32.h",
            gl_include / "GLES2" / "gl2ext.h",
        ],
    )

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="uwp_posix_stubs",
        command=f"{python} tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="UWP POSIX STUBS",
    )
    n.build(
        outputs=[guest_posix_c, posix_imports],
        rule="uwp_posix_stubs",
        implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"],
    )

    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = PORT_ANDROID_DIR / "host_imports.list"
    n.rule(
        name="uwp_imports",
        command=(f"{python} tools/uwp_imports.py --host-table {host_table_c} "
                 f"--guest-host-h {PORT_ANDROID_DIR}/guest/runtime/guest_host.h "
                 f"--posix-h {LINUX_DIR}/src/posix.h {imports_s} $in"),
        description="UWP IMPORTS",
    )
    n.build(
        outputs=[imports_s, host_table_c],
        rule="uwp_imports",
        inputs=[host_imports_list, posix_imports, gl_imports],
        implicit=[
            Path("tools/uwp_imports.py"),
            PORT_ANDROID_DIR / "guest/runtime/guest_host.h",
            LINUX_DIR / "src/posix.h",
        ],
    )

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header, platform_semantics_header]

    # ---------- guest compilation (x86_64-linux-gnux32)
    n.rule(
        name="uwp_guest_cc",
        command=f"{compile_launcher(sln)}$uwp_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="UWP GUEST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="uwp_guest_as",
        command="$uwp_cc --target=x86_64-linux-gnux32 -c $in -o $out",
        description="UWP GUEST AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}",
        f"-isystem {arch}",
        f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS + configuration_defines(sln))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = list(generated_headers)
    profile = pgo_profile(sln, None, [WINDOWS_PROFILE, LINUX_PROFILE], cc)
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/\\")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="uwp_guest_cc", inputs=source, implicit=tool_implicit, variables={"cflags": cflags})
        return obj

    # musl libc
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="uwp_ar",
        command="$uwp_ar rcs $out @$out.rsp",
        description="UWP AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="uwp_ar", inputs=musl_objects)

    # game sources
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() == "source/main/main.c":
            cflags += " " + updater_defines(getattr(sln, "port_release", False))
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {PORT_ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))
    objects.append(guest_object(STB_VORBIS_SOURCE, game_cflags))

    # platform layer + guest runtime
    sdl_include = SDL_UWP_DIR / "include"
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{PORT_ANDROID_DIR}/guest/runtime",
        f"-I{PORT_ANDROID_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{_quote(sdl_include)}", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in hud_assets_build(n, "uwp", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    for source in opus_sources():
        objects.append(guest_object(source, " ".join([opus_cflags(guest_abi), *libc_includes])))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES])))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", profile_flags, *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))

    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{PORT_ANDROID_DIR}/guest/runtime", f"-I{PORT_ANDROID_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{PORT_ANDROID_DIR}/guest/runtime", f"-I{PORT_ANDROID_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{_quote(sdl_include)}", f"-I{gl_include}", *libc_includes,
    ])
    runtime_dir = PORT_ANDROID_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="uwp_guest_as", inputs=imports_s)
    objects.append(imports_o)

    linker_script = PORT_UWP_DIR / "guest" / "guest.ld"
    n.rule(
        name="uwp_guest_link",
        command=(f"$uwp_ld -m elf32_x86_64 -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="UWP GUEST LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="uwp_guest_link", inputs=objects, implicit=[libguestc, linker_script])

    # ---------- 64-bit UWP host executable (build/uwp/halo_uwp.exe)
    host_objects: List[Path] = []
    host_obj_dir = BUILD / "host" / "obj"
    n.rule(
        name="uwp_host_cc",
        command=f"{compile_launcher(sln)}$uwp_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="UWP HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_base_cflags = " ".join([
        *HOST_CFLAGS,
        updater_defines(getattr(sln, "port_release", False)),
        f"-I{PORT_UWP_DIR}/host",
        f"-I{PORT_UWP_DIR}/include",
        f"-I{PORT_ANDROID_DIR}/include",
        f"-I{PORT_ANDROID_DIR}/host",
        f"-I{_quote(sdl_include)}",
        f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}",
        f"-I{ZLIB_DIR}",
    ])
    host_sources = sorted((PORT_UWP_DIR / "host").glob("*.c")) + [
        PORT_ANDROID_DIR / "host" / "host_watch_hash.c",
        PORT_WINDOWS_DIR / "src" / "win32_files.c",
        PORT_WINDOWS_DIR / "src" / "win32_net.c",
        LINUX_DIR / "src" / "xiso.c",
        TOML_DIR / "tomlc17.c",
    ]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="uwp_host_cc", inputs=source, variables={"cflags": host_base_cflags})
        host_objects.append(obj)

    for name in ZLIB_SOURCES:
        source = ZLIB_DIR / name
        obj = host_obj_dir / ("zlib_" + source.name + ".o")
        n.build(outputs=obj, rule="uwp_host_cc", inputs=source,
                variables={"cflags": " ".join([host_base_cflags, *ZLIB_DEFINES, "-w"])})
        host_objects.append(obj)

    miniupnpc_cflags = " ".join([
        host_base_cflags,
        f"-I{MINIUPNPC_DIR / 'include'}",
        f"-I{MINIUPNPC_DIR / 'src'}",
        *MINIUPNPC_DEFINES,
    ])
    for source in [PORT_WINDOWS_DIR / "src" / "win32_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="uwp_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "win32_upnp.c" else "")})
        host_objects.append(obj)

    mbedtls_cflags = " ".join([host_base_cflags, f"-I{MBEDTLS_DIR / 'include'}", f"-I{MBEDTLS_DIR / 'library'}"])
    for source in [LINUX_DIR / "src" / "posix_dtls.c", *sorted((MBEDTLS_DIR / "library").glob("*.c"))]:
        obj = host_obj_dir / ("mbedtls_" + source.name + ".o" if source.parent.parent == MBEDTLS_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="uwp_host_cc", inputs=source,
                variables={"cflags": mbedtls_cflags + (" -w" if source.name != "posix_dtls.c" else "")})
        host_objects.append(obj)

    for gen_src, obj_name in ((host_table_c, "host_import_table.c.o"), (host_gl_table_c, "host_gl_table.c.o")):
        obj = host_obj_dir / obj_name
        n.build(outputs=obj, rule="uwp_host_cc", inputs=gen_src, variables={"cflags": host_base_cflags + " -w"})
        host_objects.append(obj)

    sdl_lib = SDL_UWP_DIR / "lib" / "x64" / "Release" / "SDL3.lib"
    host_ldflags = " ".join([
        "--target=x86_64-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        "-Wl,/SUBSYSTEM:WINDOWS",
        "-Wl,/ENTRY:WinMainCRTStartup",
        "-Wl,/DYNAMICBASE",
        "-Wl,/HIGHENTROPYVA",
        "-Wl,/STACK:0x800000",
    ])
    host_libs = " ".join([_quote(sdl_lib), *HOST_LIBS])
    n.rule(
        name="uwp_host_link",
        command="$uwp_cc $ldflags -o $out --rsp-quoting=windows @$out.rsp $libs",
        description="UWP HOST LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(
        outputs=host_exe,
        rule="uwp_host_link",
        inputs=host_objects,
        variables={"ldflags": host_ldflags, "libs": host_libs},
    )

    # ---------- MSIX staging, packaging, and signing
    n.rule(
        name="uwp_package_msix",
        command=f"{python} tools/uwp_build.py --package-msix {appx_dir} {msix_out} {cer_out}",
        description="UWP PACKAGE MSIX $out",
        pool="console",
    )
    n.build(
        outputs=[msix_out, cer_out],
        rule="uwp_package_msix",
        inputs=[host_exe, image],
        implicit=[
            Path("tools/uwp_build.py"),
            PORT_UWP_DIR / "package" / "Package.appxmanifest.in",
            PORT_UWP_DIR / "package" / "generate_assets.py",
            Path("port/assets/network/brokers.txt"),
        ],
    )
    n.build(outputs="uwp", rule="phony", inputs=[msix_out, cer_out])
    n.newline()


# ---------- CLI helpers invoked by Ninja rules ----------

def _convert_mkalltypes(lines: List[str]) -> str:
    out: List[str] = []
    for raw in lines:
        line = raw.rstrip("\r\n")
        m_ty = re.match(r"^TYPEDEF\s+(.*\S)\s+(\w+);$", line)
        if m_ty:
            body, name = m_ty.group(1), m_ty.group(2)
            out.append(f"#if defined(__NEED_{name}) && !defined(__DEFINED_{name})")
            out.append(f"typedef {body} {name};")
            out.append(f"#define __DEFINED_{name}")
            out.append("#endif")
            continue
        m_st = re.match(r"^STRUCT\s+(\w+)\s+(.*);$", line)
        if m_st:
            name, body = m_st.group(1), m_st.group(2)
            out.append(f"#if defined(__NEED_struct_{name}) && !defined(__DEFINED_struct_{name})")
            out.append(f"struct {name} {body};")
            out.append(f"#define __DEFINED_struct_{name}")
            out.append("#endif")
            continue
        m_un = re.match(r"^UNION\s+(\w+)\s+(.*);$", line)
        if m_un:
            name, body = m_un.group(1), m_un.group(2)
            out.append(f"#if defined(__NEED_union_{name}) && !defined(__DEFINED_union_{name})")
            out.append(f"union {name} {body};")
            out.append(f"#define __DEFINED_union_{name}")
            out.append("#endif")
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def _cmd_gen_musl_headers(musl_dir: Path, arch_dir: Path, alltypes_out: Path, syscall_out: Path, version_out: Path) -> None:
    alltypes_out.parent.mkdir(parents=True, exist_ok=True)
    syscall_out.parent.mkdir(parents=True, exist_ok=True)
    version_out.parent.mkdir(parents=True, exist_ok=True)

    in_lines = (
        (arch_dir / "bits" / "alltypes.h.in").read_text(encoding="utf-8").splitlines()
        + (musl_dir / "include" / "alltypes.h.in").read_text(encoding="utf-8").splitlines()
    )
    alltypes_out.write_text(_convert_mkalltypes(in_lines), encoding="utf-8")

    syscall_in = (arch_dir / "bits" / "syscall.h.in").read_text(encoding="utf-8")
    sys_lines = [syscall_in.rstrip("\n")]
    for line in syscall_in.splitlines():
        if line.startswith("#define __NR_"):
            sys_lines.append(line.replace("#define __NR_", "#define SYS_", 1))
    syscall_out.write_text("\n".join(sys_lines) + "\n", encoding="utf-8")
    version_out.write_text(f'#define VERSION "{MUSL_VERSION_STR}"\n', encoding="utf-8")


def _find_windows_sdk_tool(name: str) -> str:
    kits_bin = Path("C:/Program Files (x86)/Windows Kits/10/bin")
    if kits_bin.is_dir():
        versions = sorted((d for d in kits_bin.iterdir() if d.is_dir() and d.name[0].isdigit()), reverse=True)
        for ver in versions:
            candidate = ver / "x64" / name
            if candidate.is_file():
                return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"Could not find {name} in Windows Kits/10/bin or PATH")


def _bundle_vclibs_app_dlls(appx_dir: Path, deps_dir: Path) -> None:
    """Extracts *_app.dll from Microsoft.VCLibs.x64.14.00.appx directly into
    appx_dir so halo-uwp.msix is 100% self-contained on Xbox Dev Mode, and also
    copies the .appx into build/uwp/dependencies/."""
    candidates = [
        Path("C:/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx"),
        Path("C:/Program Files/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx"),
    ]
    for vclibs_appx in candidates:
        if vclibs_appx.is_file():
            deps_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(vclibs_appx, deps_dir / vclibs_appx.name)
            with zipfile.ZipFile(vclibs_appx) as zf:
                for member in zf.namelist():
                    if member.lower().endswith(".dll") and "/" not in member and "\\" not in member:
                        (appx_dir / member).write_bytes(zf.read(member))
            return


def _ensure_signing_certificate(pfx_path: Path, cer_path: Path) -> str:
    password = os.environ.get("UWP_PFX_PASSWORD", "opence")
    pfx_b64 = os.environ.get("UWP_PFX_BASE64", "").strip()
    if pfx_b64:
        pfx_path.write_bytes(base64.b64decode(pfx_b64))
        if not cer_path.is_file():
            cer_path.write_bytes(b"")
        return password

    if pfx_path.is_file() and cer_path.is_file():
        return password

    ps_script = (
        "$ErrorActionPreference = 'Stop'; "
        "$cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=OpenCE' "
        "-KeyUsage DigitalSignature -FriendlyName 'OpenCE UWP Self-Signed' "
        "-CertStoreLocation 'Cert:\\CurrentUser\\My' "
        "-TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}'); "
        f"$pwd = ConvertTo-SecureString -String '{password}' -Force -AsPlainText; "
        f"Export-PfxCertificate -Cert $cert -FilePath '{pfx_path.resolve()}' -Password $pwd | Out-Null; "
        f"Export-Certificate -Cert $cert -FilePath '{cer_path.resolve()}' | Out-Null; "
        "Remove-Item $cert.PSPath -Force"
    )
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps_script], check=True)
    return password


def _cmd_package_msix(appx_dir: Path, msix_out: Path, cer_out: Path) -> None:
    if appx_dir.exists():
        shutil.rmtree(appx_dir)
    appx_dir.mkdir(parents=True, exist_ok=True)

    # 1. Stage executable, guest ELF, and brokers.txt
    shutil.copy2(BUILD / "halo_uwp.exe", appx_dir / "halo_uwp.exe")
    shutil.copy2(BUILD / "halo_guest.elf", appx_dir / "halo_guest.elf")
    shutil.copy2(ROOT / "port/assets/network/brokers.txt", appx_dir / "brokers.txt")

    # 2. Stage SDL3.dll and Mesa EGL/GLESv2->D3D12 DLLs
    shutil.copy2(SDL_UWP_DIR / "bin" / "x64" / "Release" / "SDL3.dll", appx_dir / "SDL3.dll")
    for dll in sorted((MESA_UWP_DIR / "x64").glob("*.dll")):
        shutil.copy2(dll, appx_dir / dll.name)

    # 3. Bundle VCLibs *_app.dll into appx_dir for zero-dependency Xbox Dev Mode deployment
    _bundle_vclibs_app_dlls(appx_dir, BUILD / "dependencies")

    # 4. Stage license files
    licenses = (
        ("port/third_party/extract-xiso/LICENSE.TXT", "extract-xiso-LICENSE.txt"),
        ("port/third_party/mbedtls/LICENSE", "mbedtls-LICENSE.txt"),
        ("port/third_party/miniupnpc/LICENSE", "miniupnpc-LICENSE.txt"),
        ("port/assets/fonts/Overpass-OFL.txt", "Overpass-OFL.txt"),
        ("port/third_party/expat/COPYING", "expat-COPYING.txt"),
        ("port/third_party/opus/COPYING", "opus-COPYING.txt"),
        ("port/assets/icons/lucide/LICENSE", "lucide-LICENSE.txt"),
    )
    for src_rel, dst_name in licenses:
        src_path = ROOT / src_rel
        if src_path.is_file():
            shutil.copy2(src_path, appx_dir / dst_name)

    # 5. Generate UWP visual assets
    subprocess.run([sys.executable, str(ROOT / "port/uwp/package/generate_assets.py"), str(appx_dir / "Assets")], check=True)

    # 6. Write Package.appxmanifest with build version
    build_num_str = os.environ.get("HALO_BUILD_NUMBER", "0")
    build_num = int(build_num_str) if build_num_str.isdigit() else 0
    pkg_version = f"1.0.{min(build_num, 65535)}.0"
    manifest_template = (ROOT / "port/uwp/package/Package.appxmanifest.in").read_text(encoding="utf-8")
    (appx_dir / "Package.appxmanifest").write_text(
        manifest_template.replace("@PACKAGE_VERSION@", pkg_version),
        encoding="utf-8",
    )

    # 7. Pack MSIX with MakeAppx.exe and sign with SignTool.exe
    makeappx = _find_windows_sdk_tool("makeappx.exe")
    signtool = _find_windows_sdk_tool("signtool.exe")
    if msix_out.exists():
        msix_out.unlink()
    subprocess.run([makeappx, "pack", "/d", str(appx_dir), "/p", str(msix_out), "/o", "/nv"], check=True)

    pfx_path = BUILD / "OpenCE-UWP.pfx"
    password = _ensure_signing_certificate(pfx_path, cer_out)
    subprocess.run(
        [signtool, "sign", "/fd", "SHA256", "/a", "/f", str(pfx_path), "/p", password, str(msix_out)],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gen-musl-headers", nargs=5, metavar=("MUSL_DIR", "ARCH_DIR", "ALLTYPES", "SYSCALL", "VERSION"))
    parser.add_argument("--package-msix", nargs=3, metavar=("APPX_DIR", "MSIX_OUT", "CER_OUT"))
    args = parser.parse_args()

    if args.gen_musl_headers:
        m_dir, a_dir, alltypes, syscall_h, ver_h = (Path(p) for p in args.gen_musl_headers)
        _cmd_gen_musl_headers(m_dir, a_dir, alltypes, syscall_h, ver_h)
        return 0
    if args.package_msix:
        appx_dir, msix_out, cer_out = (Path(p) for p in args.package_msix)
        _cmd_package_msix(appx_dir, msix_out, cer_out)
        return 0
    parser.error("specify --gen-musl-headers or --package-msix")
    return 1


if __name__ == "__main__":
    sys.exit(main())
