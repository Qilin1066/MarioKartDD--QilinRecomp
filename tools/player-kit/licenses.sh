# The licences every download carries (sourced by package.sh,
# package-linux.sh and package-macos.sh, which add their compiler's or
# MoltenVK's): project_licenses <LICENSES folder>, with $root (the source),
# $build (the build folder) and $PACKAGE set; crlf=1 gives the project's own
# texts Windows line ends.
#   <PACKAGE>/LICENSE.txt      this project (GPL-3.0)
#   mkdd-decomp/               third_party/mkdd (CC0 1.0): the PAL build's
#                              symbols and splits, a check on the names that
#                              placed kit/plan.tsv's libraries
#   SDL3/, Dear ImGui/, Vulkan-Headers/, volk/
#                              what the launcher and the game's runtime are built with
#   SMAA/, FidelityFX-FSR/     compiled into the runtime: SMAA's tables and
#                              shaders, FSR 1's shaders and constants
project_licenses() {
    lic=$1
    to_text() { if [ "${crlf:-0}" = 1 ]; then sed 's/$/\r/' "$1"; else cat "$1"; fi; }
    mkdir -p "$lic/$PACKAGE" "$lic/mkdd-decomp" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk" "$lic/SMAA" "$lic/FidelityFX-FSR"
    to_text "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
    to_text "$root/third_party/mkdd/LICENSE" > "$lic/mkdd-decomp/LICENSE.txt"  # CC0 1.0
    {
        echo "kit/plan.tsv, the plan of which parts of main.dol are recompiled and which"
        echo "are replaced, names the libraries in its ranges. Its names were checked"
        echo "against the symbols and splits of the Mario Kart: Double Dash!! decompilation"
        echo "(https://github.com/doldecomp/mkdd, commit 0c3baef1ba96867f4ebbcdde02f7a43624224a85),"
        echo "which the project keeps in third_party/mkdd under CC0 1.0: LICENSE.txt here."
    } > "$lic/mkdd-decomp/README.txt.tmp"
    to_text "$lic/mkdd-decomp/README.txt.tmp" > "$lic/mkdd-decomp/README.txt"
    rm "$lic/mkdd-decomp/README.txt.tmp"
    cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
    imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
    [ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
    cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
    cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
    cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"
    # (Fetched and checked by CMake: host/shaders/post/third_party.txt.)
    cp "$build/_deps/qilin_post/smaa/LICENSE.txt" "$lic/SMAA/"
    cp "$build/_deps/qilin_post/fsr1/license.txt" "$lic/FidelityFX-FSR/"
    for f in "$lic/$PACKAGE/LICENSE.txt" "$lic/mkdd-decomp/LICENSE.txt" "$lic/SDL3/LICENSE.txt" "$lic/Dear ImGui/LICENSE.txt" "$lic/volk/LICENSE.md" \
             "$lic/SMAA/LICENSE.txt" "$lic/FidelityFX-FSR/license.txt"; do
        [ -s "$f" ] || { echo "no licence text: $f" >&2; exit 1; }
    done
}
