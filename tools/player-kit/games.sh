# The game's details for the packaging scripts (sourced), as Toy Commander's
# and Punch-Out's games.sh. Everything game-specific in a download comes from
# here: the README templates' @NAME@ places (fill) and kit/game.txt
# (write_game_txt), which is all the launcher knows about the game
# (tools/player-kit/launcher/ADOPTING.md).
game=mkdd
REPO="Qilin1066/MarioKartDD--QilinRecomp"
TITLE="Double Dash"; PACKAGE="MarioKartDD--QilinRecomp"; EXE="Double Dash.exe"; BOOT="main.dol"
BUILD_TITLE="Mario Kart: Double Dash!! (USA)"; PRODUCT="GM4E01"; DISC_VERSION="0"
RELEASE="the USA release, disc GM4E01, revision 0"
STUDIO="Nintendo, GameCube, 2003"
# The game's full name, for the READMEs' prose; TITLE (the launcher's "Play
# Double Dash", the app, the menu entry) is the first downloads' "Play Double
# Dash", whose .bat, and whose game beside it (Double Dash.exe), the launcher
# then removes from an old folder (tidy.cpp).
FULL_TITLE="Mario Kart: Double Dash!!"
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@FULL_TITLE@|$FULL_TITLE|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the data it builds from (a disc image file, which the game reads at
# run time too; `build` is recomp/src/dol.cpp's known build's title, which
# `qilin identify --build` checks), how it is recompiled (three steps, as
# CMakeLists.txt's mkdd target: qilin-disc takes main.dol from the image,
# qilin lifts it with the library plan into 32 files, and qilin-dsp lifts
# the sound processor's microcode in it), what a compile needs, how the game
# starts (mkdd <image>), and where releases are published (tags vX.Y.Z).
#
# compile_memory_mb: measured, not guessed. The four largest of the 32
# files qilin lift writes from the supported main.dol (lifted_014, 015, 030
# and 031.cpp, 3.8 to 4.4 MB of C++ each) peaked at 1080 to 1460 MB of
# working set (lifted_030.cpp the most) compiled with the kit's cxxflags and
# -march=native by zig 0.17.1's clang 22 on Windows x86-64 (2026-11-28); the
# others, 2 MB or less, at 700 MB or less. 2400 leaves about 60% for the
# other compilers (llvm-mingw's, Apple's), ARM64 and a larger file after a
# lifter change. The whole output of the three steps (gen/) is 56 MB.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=file\ninput_extensions=iso;gcm;ciso\ninput_noun=disc image\n'
        printf 'input_hint=Choose your disc image: its .iso, .gcm or .ciso file. An .rvz or .wia image has to be converted first, in Dolphin (right-click the game, Convert File, ISO).\n'
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={tool:qilin-disc} dol {input} main.dol\n'
        printf 'step.2={qilin} lift main.dol -o {gen} --plan {kit}/plan.tsv --files 32\n'
        printf 'step.3={tool:qilin-dsp} lift main.dol -o {gen}/dsp_ucode.cpp\n'
        printf 'compile_memory_mb=2400\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n'
        # What the downloads before the launcher (Play Double Dash.bat and
        # kit/play.ps1, up to 0.1.6) left, and where play.ps1 remembered the
        # disc: removed, and the disc taken over (launcher 2).
        printf 'old_files=Play Double Dash.bat;kit/play.ps1;kit/ninja.exe;kit/kit.ninja;work/build.ninja;work/.ninja_log;work/.ninja_deps;'
        printf 'work/built-with.txt;work/disc-folder.txt.tmp;work/dsp_ucode.new;work/mkdd.exe;Double Dash.exe.new;Double Dash.exe.previous\n'
        printf 'old_input_file=work/disc-folder.txt\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
