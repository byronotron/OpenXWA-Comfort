#!/usr/bin/env python3
"""Recompile pinned Quest C/C++ with upstream runtime libraries and shader cache.

Shader inputs are locked to the source tag. This path deliberately rejects
shader changes; it does not pretend that copying SPIR-V compiles a changed HLSL.
All build-system adaptations live in an ignored source mirror.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd=None):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def junction(source, destination):
    if destination.exists():
        if destination.resolve() != source.resolve():
            raise RuntimeError(f"Unexpected existing source mirror path: {destination}")
        return
    if os.name == "nt":
        # Paths are arguments, never interpolated as shell syntax.
        run(["cmd", "/c", "mklink", "/J", destination, source])
    else:
        destination.symlink_to(source, target_is_directory=True)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Pinned CMake adaptation no longer matches: {old[:70]}")
    return text.replace(old, new, 1)


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--zig", type=Path, required=True, help="Portable Zig 0.13.0 executable for the Windows host tool")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--base-apk", type=Path)
    parser.add_argument("--profile", choices=("baseline", "comfort-shake", "adaptive-mask"), default="baseline")
    args = parser.parse_args()
    lock = json.loads((ROOT / "comfort/lock.json").read_text())
    for source, commit, patterns in (
        (ROOT, lock["upstream_commit"], ["src", "shaders", "third_party"] if args.profile == "baseline" else ["shaders", "third_party"]),
        (ROOT / "aeron", lock["aeron_commit"], ["src", "include", "shaders", "tools", "third_party"]),
    ):
        changed = subprocess.check_output(["git", "diff", commit, "--", *patterns], cwd=source)
        untracked = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "--", *patterns], cwd=source)
        if changed or untracked:
            raise SystemExit("Baseline source inputs differ from the pinned release; use a separately versioned development build")
    deps = ROOT / "build/deps"
    sdl = deps / "SDL"
    sdl_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=sdl, text=True).strip()
    if not sdl_commit.startswith(lock["sdl_revision"]):
        raise SystemExit("SDL headers do not match the released runtime")
    if subprocess.check_output(["git", "diff", "HEAD", "--", "include"], cwd=sdl):
        raise SystemExit("SDL headers have local modifications")
    runtime = ROOT / "build/upstream-apk/lib/arm64-v8a"
    changed_names = subprocess.check_output(["git", "diff", "--name-only", lock["upstream_commit"], "--", "src"], cwd=ROOT, text=True).splitlines()
    changed_names += subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "--", "src"], cwd=ROOT, text=True).splitlines()
    source_changes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sorted(set(changed_names))}
    shader_dir = ROOT / "build/upstream-apk/assets/shaders"
    base_apk = args.base_apk or ROOT.parent / "OpenXWA-Quest-1.0.1/OpenXWA-quest-1.0.1.apk"
    if not base_apk.exists():
        base_apk = ROOT / "build/upstream-release/OpenXWA-quest-1.0.1.apk"
    if hashlib.sha256(base_apk.read_bytes()).hexdigest() != lock["baseline_apk_sha256"]:
        raise SystemExit("Upstream APK is not the locked release")
    import zipfile
    with zipfile.ZipFile(base_apk) as archive:
        for info in archive.infolist():
            if info.filename.startswith(("assets/shaders/", "lib/")):
                local = ROOT / "build/upstream-apk" / info.filename
                if local.read_bytes() != archive.read(info):
                    raise SystemExit(f"Upstream cache changed: {info.filename}")
    ndk = args.ndk.resolve()
    if "27.2.12479018" not in (ndk / "source.properties").read_text():
        raise SystemExit("Expected Android NDK r27c (27.2.12479018)")
    zig = args.zig.resolve()
    if subprocess.check_output([str(zig), "version"], text=True).strip() != "0.13.0":
        raise SystemExit("Expected Zig 0.13.0 for the host-only generator")
    host_src = ROOT / "build/comfort-host-src"
    host_build = ROOT / "build/comfort-host"
    # Execute the upstream generator's original YAML parsing and header emitter.
    # Stop before shader compilation; cached shaders are checked separately above.
    generator = (ROOT / "aeron/tools/fsr3_shadergen/fsr3_shadergen.cpp").read_text()
    generator = replace_once(generator, "const fs::file_time_type    newestSource =", "return 0;\n\tconst fs::file_time_type    newestSource =")
    write(host_src / "generator.cpp", generator)
    yaml = ROOT / "aeron/third_party/libyaml"
    yaml_sources = "\n".join(f'"{p.as_posix()}"' for p in sorted((yaml / "src").glob("*.c")))
    write(host_src / "CMakeLists.txt", f'''cmake_minimum_required(VERSION 3.20)
project(ComfortHost C CXX)
add_library(yaml OBJECT {yaml_sources})
target_include_directories(yaml PUBLIC "{(yaml / 'include').as_posix()}" PRIVATE "{(yaml / 'src').as_posix()}")
target_compile_definitions(yaml PUBLIC YAML_DECLARE_STATIC PRIVATE YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
add_executable(comfort_shader_manifest generator.cpp)
target_compile_features(comfort_shader_manifest PRIVATE cxx_std_17)
target_include_directories(comfort_shader_manifest PRIVATE "{(sdl / 'include').as_posix()}")
target_link_libraries(comfort_shader_manifest PRIVATE yaml)
''')
    run(["cmake", "-S", host_src, "-B", host_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_C_COMPILER={zig.as_posix()}", "-DCMAKE_C_COMPILER_ARG1=cc",
         f"-DCMAKE_CXX_COMPILER={zig.as_posix()}", "-DCMAKE_CXX_COMPILER_ARG1=c++",
         f"-DCMAKE_AR={(ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ar.exe').as_posix()}",
         f"-DCMAKE_RANLIB={(ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ranlib.exe').as_posix()}"])
    run(["cmake", "--build", host_build, "--parallel", str(args.jobs)])
    generated = ROOT / "build/comfort-fsr3"
    run([host_build / "comfort_shader_manifest.exe", "--manifest", ROOT / "aeron/shaders/fsr3/fsr3_shaders.yaml",
         "--gpu-include", ROOT / "aeron/third_party/fidelityfx-fsr3/include/FidelityFX/gpu",
         "--generated-dir", generated, "--shader-output-dir", ROOT / "build/comfort-unused-shader-output",
         "--shadercross", zig, "--formats", "SPIRV"])
    mirror = ROOT / "build/comfort-native-src"
    mirror.mkdir(parents=True, exist_ok=True)
    for path in ROOT.iterdir():
        if path.is_dir() and path.name not in (".git", "build", "aeron"):
            junction(path, mirror / path.name)
    shutil.copyfile(ROOT / "CMakeLists.txt", mirror / "CMakeLists.txt")
    aeron_mirror = mirror / "aeron"
    aeron_mirror.mkdir(exist_ok=True)
    for path in (ROOT / "aeron").iterdir():
        if path.is_dir() and path.name != ".git":
            junction(path, aeron_mirror / path.name)
    cmake = (ROOT / "aeron/CMakeLists.txt").read_text()
    start = cmake.index('if(NOT EXISTS "${AERON_SHADERCROSS_EXECUTABLE}")')
    end = cmake.index("if(NOT DEFINED AERON_SHADER_FORMATS)", start)
    cmake = cmake[:start] + cmake[end:]
    start = cmake.index("    set(generated)\n", cmake.index("function(aeron_compile_hlsl"))
    end = cmake.index("endfunction()", start)
    cmake = cmake[:start] + '''    set(generated)
    foreach(hlsl IN LISTS ARGN)
        get_filename_component(base ${hlsl} NAME)
        string(REGEX REPLACE "\\\\.hlsl$" ".spv" name "${base}")
        set(cached "${COMFORT_SHADER_CACHE}/${name}")
        if(NOT EXISTS "${cached}")
            message(FATAL_ERROR "Missing pinned shader ${name}")
        endif()
        file(MAKE_DIRECTORY "${output_dir}")
        configure_file("${cached}" "${output_dir}/${name}" COPYONLY)
        list(APPEND generated "${output_dir}/${name}")
    endforeach()
    set(${out_files_var} ${generated} PARENT_SCOPE)
''' + cmake[end:]
    start = cmake.index('set(AERON_FSR3_SHADERGEN_EXECUTABLE ""')
    end = cmake.index("add_custom_target(aeron_shaders", start)
    cmake = cmake[:start] + f'''set(AERON_FSR3_GENERATED_DIR "{generated.as_posix()}")
add_custom_target(aeron_fsr3_shaders)
file(GLOB comfort_fsr_shaders "${{COMFORT_SHADER_CACHE}}/fsr3*.spv")
file(COPY ${{comfort_fsr_shaders}} DESTINATION "${{AERON_SHADER_OUTPUT_DIR}}")

''' + cmake[end:]
    # Link the released FFmpeg binaries via an imported target; headers use the
    # matching major/minor release. No film/audio feature is compiled out.
    cmake = replace_once(cmake, "    find_package(PkgConfig REQUIRED)\n    pkg_check_modules(FFMPEG_MEDIA REQUIRED IMPORTED_TARGET\n        libavformat\n        libavcodec\n        libavutil\n        libswresample\n    )", f'''    add_library(PkgConfig::FFMPEG_MEDIA INTERFACE IMPORTED)
    set_target_properties(PkgConfig::FFMPEG_MEDIA PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "{(deps / 'ffmpeg-8.0.1').as_posix()}"
        INTERFACE_LINK_LIBRARIES "{';'.join((runtime / n).as_posix() for n in ('libavformat.so','libavcodec.so','libavutil.so','libswresample.so','libswscale.so'))}")''')
    cmake = replace_once(cmake, "    pkg_check_modules(FFMPEG_VIDEO REQUIRED IMPORTED_TARGET libswscale)",
                         "    add_library(PkgConfig::FFMPEG_VIDEO ALIAS PkgConfig::FFMPEG_MEDIA)")
    write(aeron_mirror / "CMakeLists.txt", cmake)
    # Public FFmpeg avconfig contains target ABI properties, not codec features.
    write(deps / "ffmpeg-8.0.1/libavutil/avconfig.h", "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
    prefix = ROOT / "build/comfort-prefix"
    write(prefix / "include/zstd.h", (deps / "zstd.h").read_text())
    write(prefix / "include/zstd_errors.h", (deps / "zstd_errors.h").read_text())
    write(prefix / "lib/cmake/SDL3/SDL3Config.cmake", f'''set(SDL3_VERSION 3.5.0)
set(SDL3_FOUND TRUE)
set(SDL3_SDL3-shared_FOUND TRUE)
if(NOT TARGET SDL3::SDL3-shared)
add_library(SDL3::SDL3-shared SHARED IMPORTED)
set_target_properties(SDL3::SDL3-shared PROPERTIES IMPORTED_LOCATION "{(runtime / 'libSDL3.so').as_posix()}" INTERFACE_INCLUDE_DIRECTORIES "{(sdl / 'include').as_posix()}")
add_library(SDL3::SDL3 ALIAS SDL3::SDL3-shared)
endif()
''')
    write(prefix / "lib/cmake/SDL3/SDL3ConfigVersion.cmake", 'set(PACKAGE_VERSION "3.5.0")\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n')
    write(prefix / "lib/cmake/zstd/zstdConfig.cmake", f'''if(NOT TARGET zstd::libzstd)
add_library(zstd::libzstd SHARED IMPORTED)
set_target_properties(zstd::libzstd PROPERTIES IMPORTED_LOCATION "{(runtime / 'libzstd.so').as_posix()}" INTERFACE_INCLUDE_DIRECTORIES "{(prefix / 'include').as_posix()}")
endif()
''')
    build = ROOT / "build/comfort-native"
    run(["cmake", "-S", mirror, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={(ndk / 'build/cmake/android.toolchain.cmake').as_posix()}",
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29", "-DANDROID_STL=c++_shared",
         f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}", f"-DCOMFORT_SHADER_CACHE={shader_dir.as_posix()}",
         f"-DSDL3_DIR={(prefix / 'lib/cmake/SDL3').as_posix()}",
         f"-Dzstd_DIR={(prefix / 'lib/cmake/zstd').as_posix()}",
         f"-DAERON_VULKAN_INCLUDE_DIR={(ROOT / 'third_party/vulkan-headers').as_posix()}",
         f"-DAERON_SDL_OPENXR_HEADER={(sdl / 'include/SDL3/SDL_openxr.h').as_posix()}",
         "-DXWA_BUILD_TOOLS=OFF", "-DXWA_ENABLE_DEBUG_UI=OFF", "-DXWA_ENABLE_VIDEO=ON", "-DAERON_ENABLE_OPENXR=ON",
         "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
         "-DXWA_VERSION=1.0.1", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"])
    run(["cmake", "--build", build, "--target", "xwa", "--parallel", str(args.jobs)])
    packaged = build / "packaged"
    packaged.mkdir(exist_ok=True)
    strip = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
    libraries = {}
    for name in ("libmain.so", "libopenxwa_vkshim.so"):
        target = packaged / name
        shutil.copyfile(build / name, target)
        run([strip, "--strip-unneeded", target])
        libraries[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    provenance = {"source": lock, "kind": "native-source-baseline-with-upstream-dependencies",
                  "native_libraries": libraries, "shader_inputs": "pinned upstream source; unmodified cached SPIR-V",
                  "ndk": "27.2.12479018", "compiler": "NDK Clang", "video": True, "openxr": True,
                  "comfort_changes": args.profile != "baseline", "source_changes": source_changes,
                  "runtime_validation": "pending headset"}
    if args.profile == "comfort-shake":
        provenance.update({"kind": "comfort-camera-shake-test", "app_version": "0.1.2-original-default",
                           "app_version_code": 4, "app_label": "OpenXWA Comfort", "default_impact_shake": 1.0})
    if args.profile == "adaptive-mask":
        provenance.update({"kind": "comfort-adaptive-mask-test", "app_version": "0.2.5-mask-strength",
                           "app_version_code": 10, "app_label": "OpenXWA Comfort", "default_impact_shake": 1.0,
                           "default_peripheral_mask": False,
                           "build_definition_sha256": hashlib.sha256((ROOT / "CMakeLists.txt").read_bytes()).hexdigest()})
    write(packaged / "native-build.json", json.dumps(provenance, indent=2))
    print(f"Native libraries rebuilt: {build}")


if __name__ == "__main__":
    main()
