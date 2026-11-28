# Exbit (C++): one `make` builds the whole program, with nothing to fetch.
#
#   make            build ./exbit (release, -O2)
#   make DEBUG=1    build ./exbit-debug with -O0 -g and sanitizers (objects in build-debug/)
#   make test       build and run the unit tests (tests/*.cpp), the worst-case runs too (tests/test_stress.cpp)
#   make hooks      run `make test` before every git push (tools/pre-push; --no-verify skips it once)
#   make appimage   build build/appimage/Exbit-<YEAR.MONTH.DAY.HOUR.MINUTE.SECOND>-x86_64.AppImage
#   make publish    the same, published as a GitHub release (before every push; tools/appimage.sh)
#   make clean
#
# Needs only a C++17 compiler and the system's own libraries: libdl and pthreads (plugins, threads), and to
# build, X11's, ALSA's and PulseAudio's headers: their libraries (the windows, sound devices, the sound server)
# are loaded when used (platform/x11_api.h, audio/alsa_api.h, audio/devices_pulse.cpp), so the program runs without them
# (exbit --tui on a text console). Everything else (the GUI toolkit,
# font rendering, SVG and PNG, WAV/AIFF/FLAC/MP3/Ogg Vorbis, MIDI files, JSON, VST2 and VST3
# hosting) is Exbit's own code in src/.
#
# With Wine's winegcc installed, it also builds exbit-wine-host.exe (src/winehost: the host
# that runs Windows plugins under Wine, plugins/bridge.h), on Exbit's own Win32 declarations
# (src/winehost/include); without it, Windows plugins say they need it. WINEGCC=... picks another.
# With 32-bit build support (gcc-multilib, g++-multilib), also exbit-wine-host-32.exe, for 32-bit plugins;
# where Wine is built only for WoW64 (no 32-bit Unix part), that host is built with MinGW's
# i686-w64-mingw32-g++ instead, as a plain Windows program (src/winehost/unixside.h).
#
# On Windows (MSYS2's UCRT64 shell: pacman -S make mingw-w64-ucrt-x86_64-gcc), the same `make` builds exbit.exe,
# linked statically (it needs none of MSYS2's DLLs, only Windows' own), and `make tests-win` the tests that run
# there. One source tree: Windows-only code is in files named *_win.cpp / *_win.h (tiny differences:
# #ifdef _WIN32 in place; in the files the Wine host shares, EXBIT_WINDOWS, as _WIN32 means the Wine host there),
# left out of the Linux build; Linux-only code (LINUX_ONLY below, *_posix.cpp) is left out of the Windows one.

CXX      ?= g++
TARGET   := exbit
BUILD    := build

ifeq ($(OS),Windows_NT)
  WINDOWS := 1
  EXE := .exe
endif

ifeq ($(DEBUG),1)
  OPT := -O0 -g $(if $(WINDOWS),,-fsanitize=address,undefined)
  LDOPT := $(if $(WINDOWS),,-fsanitize=address,undefined)
  BUILD := build-debug
  TARGET := exbit-debug
else
  OPT := -O2 -g1 -DNDEBUG
  LDOPT :=
endif
TARGET := $(TARGET)$(EXE)

CXXFLAGS ?=
CXXFLAGS += -std=c++17 $(OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -MMD -MP -Isrc -pthread -DEXBIT_COMMIT='"$(shell git rev-parse HEAD 2>/dev/null)"'
LDLIBS   := -ldl -pthread -lm

SRCS  := $(shell find src -name '*.cpp' -not -path 'src/winehost/*' | sort)
# the sources of one system only
LINUX_ONLY := src/platform/x11.cpp src/platform/x11_api.cpp src/audio/alsa_api.cpp src/audio/devices.cpp src/audio/devices_alsa.cpp \
              src/audio/devices_jack.cpp src/audio/devices_pulse.cpp src/audio/midi_jack.cpp src/plugins/wine.cpp \
              src/plugins/wine_prefix_fixes.cpp src/plugins/installer_activity.cpp \
              $(filter %_posix.cpp,$(SRCS))
ifeq ($(WINDOWS),1)
  SRCS := $(filter-out $(LINUX_ONLY),$(SRCS))
  # (M_PI…; UTF-8 paths and POSIX's small calls: core/posix_win.h; off_t 64-bit: files over 2 GB)
  CXXFLAGS += -DEXBIT_WINDOWS -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -include core/posix_win.h
  # static: none of MSYS2's DLLs needed; binmode.o: files the C library opens are binary
  LDLIBS := -static -pthread $(shell $(CXX) -print-file-name=binmode.o) -luser32 -lgdi32 -lshell32 -lole32 \
            -loleaut32 -luuid -limm32 -lwinmm -lavrt -ldwmapi -lshcore -lsynchronization -lws2_32 -lm
  # the manifest (UTF-8 code page, per-monitor DPI awareness), the icon and the version details, as resources
  WIN_RES := $(BUILD)/src/platform/exbit_win.res.o
else
  SRCS := $(filter-out %_win.cpp,$(SRCS))
endif
OBJS  := $(SRCS:%.cpp=$(BUILD)/%.o)
LIB_OBJS := $(filter-out $(BUILD)/src/main.o,$(OBJS))

TEST_SRCS := $(wildcard tests/*.cpp)
ifneq ($(WINDOWS),1)
  TEST_SRCS := $(filter-out %_win.cpp,$(TEST_SRCS))
endif
TEST_BINS := $(TEST_SRCS:tests/%.cpp=$(BUILD)/tests/%$(EXE))

# test plugins (built from tests/plugins/: a VST2 synth and a VST3 effect, on Exbit's own headers)
TEST_PLUGINS := $(BUILD)/test-plugins/TinyTestSynth.so \
                $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so

# --- the Wine host (Linux code inside Wine: built with $(CXX) for Windows' calling convention,
# linked by winegcc against Wine's DLLs) ---
WINEGCC  ?= $(shell command -v winegcc 2>/dev/null)
WINE_HOST := exbit-wine-host.exe
WINE_SRCS := src/winehost/host.cpp src/winehost/platform.cpp src/winehost/d2dfix.cpp src/winehost/dragdrop.cpp src/plugins/vst3host.cpp src/plugins/vst2host.cpp \
             src/plugins/plugin.cpp src/plugins/probe.cpp src/core/util.cpp src/core/json.cpp
WINE_OBJS := $(WINE_SRCS:%.cpp=build/winehost/%.o)  # (one host for both builds, always optimized)
WINE_BASE := -std=c++17 -O2 -g1 -DNDEBUG -fPIC -Wall -Wextra \
             -Wno-unused-parameter -Wno-missing-field-initializers -MMD -MP -Isrc/winehost/include -Isrc \
             -D_WIN32 -DEXBIT_WINE_HOST
WINE_FLAGS := $(WINE_BASE) -D_WIN64 '-D__stdcall=__attribute__((ms_abi))' '-D__cdecl=__attribute__((ms_abi))'
# the 32-bit host, for 32-bit plugins (winegcc -m32: needs gcc-multilib/g++-multilib and Wine's i386 part;
# the same sources, Windows' 32-bit calling conventions)
WINE_HOST32 := exbit-wine-host-32.exe
WINE32_OBJS := $(WINE_SRCS:%.cpp=build/winehost32/%.o)
WINE32_FLAGS := $(WINE_BASE) -m32 '-D__stdcall=__attribute__((stdcall))' '-D__cdecl=__attribute__((cdecl))'
# built only when winegcc can build 32-bit programs (a tiny one links): a Wine built only for WoW64 has no
# 32-bit Unix part; then 32-bit plugins say they need this host, and the rest builds as usual
ifneq ($(WINEGCC),)
  WINE32_OK := $(shell mkdir -p build/wine32check && printf 'int main(void){return 0;}\n' > build/wine32check/t.c && \
                 $(WINEGCC) -m32 -mconsole -o build/wine32check/t.exe build/wine32check/t.c -lkernel32 >/dev/null 2>&1 && echo yes)
  ALL_WINE := $(WINE_HOST) $(if $(WINE32_OK),$(WINE_HOST32))
endif

# --- Windows builds of the test plugins, and a test installer (for test_wine; with MinGW's
# x86_64-w64-mingw32-g++ installed, e.g. the g++-mingw-w64-x86-64 package; MINGW64=... picks another),
# and 32-bit ones with i686-w64-mingw32-g++ (g++-mingw-w64-i686; MINGW32=...). Linked statically: a
# Wine prefix has no MinGW runtime DLLs. Not in test-plugins/: the Linux tests list that folder ---
MINGW64  ?= $(shell command -v x86_64-w64-mingw32-g++ 2>/dev/null)
MINGW32  ?= $(shell command -v i686-w64-mingw32-g++ 2>/dev/null)
WIN_DIR := $(BUILD)/win-test-plugins
WIN_FLAGS := -std=c++17 -O2 -static -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc
WIN_TEST_PLUGINS := $(WIN_DIR)/TinyTestFx.vst3 $(WIN_DIR)/TinyTestFx.moduleinfo.json \
                    $(WIN_DIR)/TinyTestSynth.dll $(WIN_DIR)/TinyTestInstaller.exe
WIN32_TEST_PLUGINS := $(WIN_DIR)/TinyTestFx32.vst3 $(WIN_DIR)/TinyTestSynth32.dll
# the 32-bit host as a plain Windows program, where winegcc can't build 32-bit ones and MinGW is there: its
# Linux calls go to a Linux library of its own (winehost/unixside.h), reached through a builtin DLL
WINE_UNIX32 := wine32/i386-windows/exbit-unix32.dll wine32/x86_64-unix/exbit-unix32.so
PE32_OBJS := $(WINE_SRCS:%.cpp=build/winehostpe32/%.o)
PE32_FLAGS := $(filter-out -fPIC -D_WIN32,$(WINE_BASE)) -DEXBIT_WINE_PE
ifneq ($(WINEGCC),)
  ifeq ($(WINE32_OK),)
    ifneq ($(MINGW32),)
      WINE_PE32 := yes
      ALL_WINE += $(WINE_HOST32) $(WINE_UNIX32)
    endif
  endif
endif
ifneq ($(MINGW64),)
  ALL_WIN_TEST := $(WIN_TEST_PLUGINS)
  ifneq ($(MINGW32),)
    ALL_WIN_TEST += $(WIN32_TEST_PLUGINS)
  endif
endif

# --- Exbit's Wine fixes (src/winefix: stand-ins for Wine's msi.dll and dxgi.dll that Exbit puts in its
# prefixes, plugins/wine_prefix_fixes.h), built with MinGW (C, linked statically), 64-bit and, with
# i686-w64-mingw32-g++, 32-bit; without MinGW, prefixes are left as Wine makes them ---
WINEFIX_DIR := wine-fixes
WINEFIX_COMMON := src/winefix/wine_own_dll.c
WINEFIX_FLAGS := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc
WINEFIX_CC64 := $(subst g++,gcc,$(MINGW64))
WINEFIX_CC32 := $(subst g++,gcc,$(MINGW32))
WINEFIX_LINK := -static -static-libgcc -shared
WINEFIX64 := $(WINEFIX_DIR)/x86_64-windows/msi.dll $(WINEFIX_DIR)/x86_64-windows/dxgi.dll $(WINEFIX_DIR)/exbit-wine-fix-check.exe
WINEFIX32 := $(WINEFIX_DIR)/i386-windows/msi.dll $(WINEFIX_DIR)/i386-windows/dxgi.dll $(WINEFIX_DIR)/exbit-wine-fix-check-32.exe
ifneq ($(WINDOWS),1)
  ifneq ($(MINGW64),)
    ALL_WINEFIX := $(WINEFIX64) $(if $(MINGW32),$(WINEFIX32))
  endif
endif
# the test programs of the fixes (tests/winefix), run in a prefix by tests/test_wine_fixes.cpp
WINEFIX_TESTS := $(WIN_DIR)/winefix_msi_commit.exe $(WIN_DIR)/winefix_vblank.exe
ifneq ($(ALL_WINEFIX),)
  ALL_WIN_TEST += $(WINEFIX_TESTS)
endif

# --- Windows: the plugin hosts. The Wine host's code built as Windows programs (EXBIT_PLUGIN_HOST): each plugin
# runs out of process in one, started directly (plugins/wine_win.cpp, bridge_win.h). exbit-plugin-host.exe, and
# exbit-plugin-host-32.exe for 32-bit plugins where a 32-bit MinGW is installed (MSYS2: pacman -S
# mingw-w64-i686-gcc; MINGW32=... picks another); without it 32-bit plugins say they need it ---
ifeq ($(WINDOWS),1)
  MINGW64 := $(CXX)
  # (MINGW32 is already set above, to the cross compiler's name that MSYS2 doesn't have: only an empty one is replaced)
  ifeq ($(strip $(MINGW32)),)
    MINGW32 := $(firstword $(wildcard /mingw32/bin/g++.exe))
  endif
  # its compiler's programs need its own DLLs (libgmp and the rest, 32-bit) first on the PATH, or they fail silently
  MINGW32_ENV := PATH="$(dir $(MINGW32)):$$$$PATH"
  PLUGIN_HOST := exbit-plugin-host.exe
  PLUGIN_HOST32 := exbit-plugin-host-32.exe
  HOST_SRCS := $(filter-out src/winehost/d2dfix.cpp src/winehost/dragdrop.cpp,$(WINE_SRCS)) src/winehost/native_win.cpp \
               src/core/os_win.cpp src/core/posix_win.cpp
  ALL_HOSTS := $(PLUGIN_HOST) $(if $(MINGW32),$(PLUGIN_HOST32))
  ALL_WIN_TEST := $(WIN_TEST_PLUGINS) $(if $(MINGW32),$(WIN32_TEST_PLUGINS))
endif
# (the hosts' flags: also the plugin speed benchmark's reference's, below, built with MinGW on Linux too)
HOST_FLAGS := -std=c++17 -O2 -g1 -DNDEBUG -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -MMD -MP \
              -Isrc -DEXBIT_PLUGIN_HOST -DEXBIT_WINDOWS -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 \
              -include core/posix_win.h
HOST_LIBS := -static -mconsole -lole32 -luser32 -lgdi32 -ladvapi32 -lws2_32 -luuid -lshell32 -lsynchronization

# --- the plugin speed benchmark (tests/test_plugin_speed.cpp): its own code (tests/plugin_speed), and its bare
# reference for Windows plugins, plugin-speed-reference.exe (the plugins called directly, on Exbit's VST3 host built
# for Windows, as exbit-plugin-host.exe), built with MinGW and run under Wine or on Windows; without MinGW, the
# benchmark of Windows plugins is skipped ---
SPEED_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,tests/plugin_speed/direct_calls.cpp tests/plugin_speed/paced_buffers.cpp \
              tests/plugin_speed/buffer_stats.cpp tests/plugin_speed/mixer_calls.cpp tests/plugin_speed/reference_program.cpp \
              tests/plugin_speed/benchmark_audio_thread.cpp tests/plugin_speed/floor_round_trip.cpp)
SPEED_REFERENCE := $(BUILD)/plugin-speed-reference.exe
SPEED_REFERENCE_SRCS := tests/plugin_speed/reference_main.cpp tests/plugin_speed/reference_thread_priority.cpp \
                        tests/plugin_speed/direct_calls.cpp tests/plugin_speed/paced_buffers.cpp src/audio/workers.cpp \
                        src/plugins/vst3host.cpp src/plugins/vst2host.cpp src/plugins/plugin.cpp src/plugins/probe.cpp \
                        src/core/util.cpp src/core/json.cpp src/core/os_win.cpp src/core/posix_win.cpp src/winehost/platform.cpp
SPEED_REFERENCE_OBJS := $(SPEED_REFERENCE_SRCS:%.cpp=build/speedreference/%.o)
ifneq ($(MINGW64),)
  ALL_SPEED_REFERENCE := $(SPEED_REFERENCE)
endif

.PHONY: all clean test test-affected tests-win hooks appimage publish plugins winehost winefixes win-plugins

all: $(TARGET) $(ALL_WINE) $(ALL_HOSTS) $(ALL_WINEFIX)

ifeq ($(WINDOWS),1)
# (the manifest too: UTF-8 the code page, as the program's, for the paths the host is given)
$(PLUGIN_HOST): $(HOST_SRCS:%.cpp=build/pluginhost/%.o) $(WIN_RES)
	$(CXX) -o $@ $^ $(HOST_LIBS)

build/pluginhost/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_FLAGS) -c $< -o $@

$(PLUGIN_HOST32): $(HOST_SRCS:%.cpp=build/pluginhost32/%.o) build/pluginhost32/manifest.res.o
	$(MINGW32_ENV) $(MINGW32) -o $@ $^ $(HOST_LIBS)

build/pluginhost32/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(MINGW32) $(HOST_FLAGS) -c $< -o $@

build/pluginhost32/manifest.res.o: src/platform/exbit_win.rc src/platform/exbit_win.manifest src/platform/exbit_win.ico
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(dir $(MINGW32))windres -I src/platform -O coff -o $@ $<
endif

hooks:
	ln -sf ../../tools/pre-push .git/hooks/pre-push
	@# (the hook it replaced, which ran the tests on every commit: gone)
	@if [ "$$(readlink .git/hooks/pre-commit 2>/dev/null)" = ../../tools/pre-commit ]; then rm -f .git/hooks/pre-commit; fi

appimage:
	tools/appimage.sh

publish:
	tools/appimage.sh --publish

winehost: $(ALL_WINE) $(ALL_WINEFIX)

winefixes: $(ALL_WINEFIX)

.SECONDARY: build/winefix/msi.def build/winefix/dxgi.def
# a stand-in's .def: each export of Wine's DLL (src/winefix/*_exports.h) is the jump thunk<index>, with
# Wine's ordinal; and ExbitWineFixReady, for the check
build/winefix/%.def: src/winefix/%_exports.h
	@mkdir -p $(dir $@)
	{ echo 'LIBRARY $*.dll'; echo EXPORTS; \
	  sed -n 's/^EXPORT(\([0-9]*\), *\([0-9]*\), *\([A-Za-z0-9_]*\))$$/    \3=thunk\1 @\2/p' $<; \
	  echo '    ExbitWineFixReady=exbitWineFixReady'; } > $@

$(WINEFIX_DIR)/x86_64-windows/%.dll: src/winefix/%_fix.c build/winefix/%.def $(WINEFIX_COMMON) src/winefix/vblank_wait.c src/winefix/*.h
	@mkdir -p $(dir $@)
	$(WINEFIX_CC64) $(WINEFIX_FLAGS) $(WINEFIX_LINK) -o $@ $< $(WINEFIX_COMMON) $(if $(filter dxgi,$*),src/winefix/vblank_wait.c) \
	    build/winefix/$*.def

$(WINEFIX_DIR)/i386-windows/%.dll: src/winefix/%_fix.c build/winefix/%.def $(WINEFIX_COMMON) src/winefix/vblank_wait.c src/winefix/*.h
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(WINEFIX_CC32) $(WINEFIX_FLAGS) $(WINEFIX_LINK) -o $@ $< $(WINEFIX_COMMON) \
	    $(if $(filter dxgi,$*),src/winefix/vblank_wait.c) build/winefix/$*.def

$(WINEFIX_DIR)/exbit-wine-fix-check.exe: src/winefix/fix_check.c
	@mkdir -p $(dir $@)
	$(WINEFIX_CC64) $(WINEFIX_FLAGS) -static -mconsole -o $@ $<

$(WINEFIX_DIR)/exbit-wine-fix-check-32.exe: src/winefix/fix_check.c
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(WINEFIX_CC32) $(WINEFIX_FLAGS) -static -mconsole -o $@ $<

# (32-bit, as most installers that change their .msi; the VBlank one 64-bit, as JUCE 8 programs)
$(WIN_DIR)/winefix_msi_commit.exe: tests/winefix/msi_commit.c
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(or $(WINEFIX_CC32),$(WINEFIX_CC64)) $(WINEFIX_FLAGS) -static -mconsole -o $@ $< -lmsi

$(WIN_DIR)/winefix_vblank.exe: tests/winefix/vblank.c
	@mkdir -p $(dir $@)
	$(WINEFIX_CC64) $(WINEFIX_FLAGS) -static -mconsole -o $@ $< -ldxgi -luuid

ifeq ($(WINE_PE32),yes)
$(WINE_HOST32): $(PE32_OBJS)
	$(MINGW32) -static -mconsole -o $@ $^ -lole32 -luser32 -ladvapi32 -lkernel32
else
$(WINE_HOST32): $(WINE32_OBJS)
	$(WINEGCC) -m32 -mconsole -o $@ $^ -lstdc++ -lm -lpthread -lole32 -luser32 -ladvapi32 -lkernel32 -Wl,-z,defs
endif

build/winehostpe32/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(MINGW32) $(PE32_FLAGS) -c $< -o $@

# (built by winegcc on Wine's own headers, and marked builtin: only a builtin DLL may call into its Unix part)
wine32/i386-windows/exbit-unix32.dll: src/winehost/unix32.c src/winehost/exbit-unix32.spec
	@mkdir -p $(dir $@)
	$(WINEGCC) -b i686-windows -shared -include windows.h -include $(dir $(WINEGCC))../include/wine/unixlib.h \
	    -o $@ src/winehost/exbit-unix32.spec $<
	$(dir $(WINEGCC))winebuild --builtin $@

wine32/x86_64-unix/exbit-unix32.so: src/winehost/unixside.c src/winehost/unixside.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -fPIC -shared -Isrc -o $@ $<

build/winehost32/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WINE32_FLAGS) -c $< -o $@

$(WINE_HOST): $(WINE_OBJS)
	$(WINEGCC) -mconsole -o $@ $^ -lstdc++ -lm -lpthread -lole32 -luser32 -ladvapi32 -lkernel32 -Wl,-z,defs

build/winehost/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WINE_FLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(WIN_RES)
	$(CXX) $(LDOPT) -o $@ $^ $(LDLIBS)

$(BUILD)/src/platform/exbit_win.res.o: src/platform/exbit_win.rc src/platform/exbit_win.manifest src/platform/exbit_win.ico
	@mkdir -p $(dir $@)
	windres -I src/platform -O coff -o $@ $<

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# the program's own stylesheet, built in (ui/theme.cpp: used where data/skins/default.css can't be read)
$(BUILD)/gen/default_css.inc: data/skins/default.css
	@mkdir -p $(dir $@)
	{ printf 'R"exbitcss('; cat $<; printf ')exbitcss"\n'; } > $@
$(BUILD)/src/ui/theme.o: $(BUILD)/gen/default_css.inc
$(BUILD)/src/ui/theme.o: CXXFLAGS += -I$(BUILD)/gen

# (test_wine looks at plugins' windows itself, through Xlib: linked to it, as the program no longer is)
$(BUILD)/tests/test_wine: LDLIBS += -lX11 -lXrandr
# (test_jack_midi makes ALSA MIDI ports itself: linked to libasound, as the program no longer is)
$(BUILD)/tests/test_jack_midi: LDLIBS += -lasound

$(BUILD)/tests/%$(EXE): tests/%.cpp $(LIB_OBJS) $(WIN_RES)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDOPT) -o $@ $< $(LIB_OBJS) $(WIN_RES) $(LDLIBS)

# (test_plugin_speed: with its own code, tests/plugin_speed)
$(BUILD)/tests/test_plugin_speed$(EXE): $(SPEED_OBJS)
$(BUILD)/tests/test_plugin_speed$(EXE): LIB_OBJS += $(SPEED_OBJS)
# (test_bridge_round_trip: the benchmark's pacing and its floor)
ROUND_TRIP_OBJS := $(BUILD)/tests/plugin_speed/paced_buffers.o $(BUILD)/tests/plugin_speed/floor_round_trip.o
$(BUILD)/tests/test_bridge_round_trip$(EXE): $(ROUND_TRIP_OBJS)
$(BUILD)/tests/test_bridge_round_trip$(EXE): LIB_OBJS += $(ROUND_TRIP_OBJS)

$(SPEED_REFERENCE): $(SPEED_REFERENCE_OBJS)
	$(MINGW64) -o $@ $^ $(HOST_LIBS) -lavrt -lwinmm

build/speedreference/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(MINGW64) $(HOST_FLAGS) -c $< -o $@

# the tests that run on Windows (tools/wintests.sh builds and runs them there)
WIN_TESTS := test_os test_gfx test_update_check test_icon_win test_model test_project_io test_ui_main test_workers test_midi_win test_wasapi_win \
             test_tui test_tui_console_win test_plugins_win test_window_win test_plugin_restore test_discord_presence test_old_name \
             test_plugin_speed test_realtime_priority test_bridge_round_trip
tests-win: $(WIN_TESTS:%=$(BUILD)/tests/%$(EXE)) $(ALL_HOSTS) $(ALL_WIN_TEST) $(ALL_SPEED_REFERENCE)
	@failed=""; for t in $(WIN_TESTS:%=$(BUILD)/tests/%$(EXE)); do echo "== $$t"; \
	    EXBIT_PLUGIN_HOST=$(PLUGIN_HOST) EXBIT_PLUGIN_HOST32=$(if $(MINGW32),$(PLUGIN_HOST32)) \
	    EXBIT_WIN_TEST_PLUGINS=$(WIN_DIR) $$t || failed="$$failed $${t##*/}"; done; \
	if [ -n "$$failed" ]; then echo "== FAILED:$$failed"; exit 1; fi; echo "== all tests passed"

plugins: $(TEST_PLUGINS)

$(BUILD)/test-plugins/TinyTestSynth.so: tests/plugins/test_vst2_synth.cpp src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -fPIC -shared -Isrc -o $@ $< -lX11

$(BUILD)/test-plugins/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so: tests/plugins/test_vst3_fx.cpp src/plugins/vst3_api.h tests/plugins/TinyTestFx.moduleinfo.json
	@mkdir -p $(dir $@) $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/Resources
	$(CXX) -std=c++17 -O2 -fPIC -shared -Isrc -o $@ $< -lX11
	cp tests/plugins/TinyTestFx.moduleinfo.json $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/Resources/moduleinfo.json

win-plugins: $(ALL_WIN_TEST)

$(WIN_DIR)/TinyTestFx.vst3: tests/plugins/test_vst3_fx.cpp tests/plugins/win_dnd.h src/plugins/vst3_api.h
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -shared -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestFx.moduleinfo.json: tests/plugins/TinyTestFx.moduleinfo.json
	@mkdir -p $(dir $@)
	cp $< $@

$(WIN_DIR)/TinyTestSynth.dll: tests/plugins/test_vst2_synth.cpp tests/plugins/win_dnd.h src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -shared -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

# (32-bit: --kill-at gives GetPluginFactory, a __stdcall function, its plain name, as Windows hosts look for it)
$(WIN_DIR)/TinyTestFx32.vst3: tests/plugins/test_vst3_fx.cpp tests/plugins/win_dnd.h src/plugins/vst3_api.h
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(MINGW32) $(WIN_FLAGS) -shared -Wl,--kill-at -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestSynth32.dll: tests/plugins/test_vst2_synth.cpp tests/plugins/win_dnd.h src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(MINGW32_ENV) $(MINGW32) $(WIN_FLAGS) -shared -Wl,--kill-at -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestInstaller.exe: tests/plugins/test_installer.cpp
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -o $@ $<

# TESTS= runs only these (test-affected picks them); every test by default
TESTS ?= $(TEST_BINS)
test: $(TEST_BINS) $(TEST_PLUGINS) $(ALL_WINE) $(ALL_WINEFIX) $(ALL_WIN_TEST) $(ALL_SPEED_REFERENCE)
	@failed=""; for t in $(TESTS); do echo "== $$t"; \
	    EXBIT_TEST_PLUGINS=$(BUILD)/test-plugins EXBIT_WIN_TEST_PLUGINS=$(WIN_DIR) $$t || failed="$$failed $${t##*/}"; done; \
	if [ -n "$$failed" ]; then echo "== FAILED:$$failed"; exit 1; fi; echo "== all tests passed"

# benchmarks: timed against the machine's own state (other programs, its power settings), so run by hand
# (make test TESTS=build/tests/test_plugin_speed), not by test-affected and the pre-push hook
BENCHMARKS := $(BUILD)/tests/test_plugin_speed$(EXE)
# only the tests the change from commit BASE reaches (tools/affected_tests.sh), benchmarks left out; every test if it
# can't tell
test-affected: $(TEST_BINS) $(TEST_PLUGINS) $(ALL_WINE) $(ALL_WINEFIX) $(ALL_WIN_TEST) $(ALL_SPEED_REFERENCE)
	@t=$$(tools/affected_tests.sh $(BASE)) || t="$(TEST_BINS)"; \
	t=$$(for x in $$t; do case " $(BENCHMARKS) " in *" $$x "*) echo "== $$x: a benchmark, run by hand" >&2 ;; *) echo $$x ;; esac; done); \
	t=$$(echo $$t); \
	if [ -z "$$t" ]; then echo "== no test reaches this change"; \
	else echo "== $$(echo $$t | wc -w) of $(words $(TEST_BINS)) tests reach this change"; \
	    $(MAKE) --no-print-directory test TESTS="$$t"; fi

clean:
	rm -rf build build-debug exbit exbit-debug exbit.exe exbit-debug.exe $(WINE_HOST) $(WINE_HOST).so $(WINE_HOST32) $(WINE_HOST32).so wine32 $(WINEFIX_DIR)

-include $(OBJS:.o=.d) $(TEST_BINS:%=%.d) $(WINE_OBJS:.o=.d) $(WINE32_OBJS:.o=.d) $(PE32_OBJS:.o=.d) $(SPEED_OBJS:.o=.d) \
         $(SPEED_REFERENCE_OBJS:.o=.d)
