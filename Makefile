CXX = x86_64-w64-mingw32-g++-posix
CC  = x86_64-w64-mingw32-gcc-posix
WINDRES = x86_64-w64-mingw32-windres
OPT = -O2 -g1
# stack overflow detection and checked string/memory functions
HARDEN = -fstack-protector-strong -D_FORTIFY_SOURCE=2
# -MMD -MP: every object also records the headers it used, so a changed header rebuilds it
DEPFLAGS = -MMD -MP
CXXFLAGS = $(OPT) $(HARDEN) $(DEPFLAGS) -std=c++17 -Icore/include -Ideps/include/libusb-1.0 -DBACKEND_NAME=genesys -D_WIN32_WINNT=0x0A00 -Wno-attributes
CFLAGS = $(OPT) $(HARDEN) $(DEPFLAGS) -Icore/include -DBACKEND_NAME=genesys

CORE_SRCS = $(wildcard core/genesys/*.cpp) core/shim/sanei_win.cpp core/shim/lide50_sim.cpp core/shim/lide50_diag.cpp
CORE_OBJS = $(patsubst %.cpp,build/%.o,$(CORE_SRCS)) build/core/shim/sanei_constrain_value.o build/core/shim/sane_strstatus.o

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/libgenesys_lide50.a: $(CORE_OBJS)
	x86_64-w64-mingw32-ar rcs $@ $^

core: build/libgenesys_lide50.a
.PHONY: core

APP_SRCS = app/main.cpp app/scanner_engine.cpp app/problems.cpp app/process.cpp app/writers.cpp app/encoders.cpp app/sysdll.cpp app/winusb_setup.cpp app/log.cpp app/ui/gfx.cpp app/ui/ui.cpp
APP_OBJS = $(patsubst %.cpp,build/%.o,$(APP_SRCS)) build/third_party/miniz.o build/app/app_res.o
APP_CXXFLAGS = $(OPT) $(HARDEN) $(DEPFLAGS) -std=c++17 -Icore/include -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wextra -Wno-unknown-pragmas -Wno-cast-function-type

build/app/%.o: app/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(APP_CXXFLAGS) -c $< -o $@

build/third_party/miniz.o: third_party/miniz.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) $(HARDEN) $(DEPFLAGS) -c $< -o $@

build/app/app_res.o: app/app.rc app/app.ico app/app.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) -i app/app.rc -o $@ --include-dir app

dist/LiDE50Scan.exe: $(APP_OBJS) build/libgenesys_lide50.a deps/lib/libusb-1.0.a
	@mkdir -p dist
	@# d2d1, dwrite, dwmapi and newdev are deliberately not linked: app/sysdll.cpp loads them
	@# from System32 at run time (see the comment there)
	$(CXX) -municode -mwindows -static -Wl,--dynamicbase,--nxcompat,--high-entropy-va -Wl,--stack,8388608 -Wl,-Map,build/LiDE50Scan.map -o build/LiDE50Scan.full.exe $(APP_OBJS) build/libgenesys_lide50.a deps/lib/libusb-1.0.a \
	  -lsetupapi -lcomdlg32 -lshell32 -lole32 -luuid -lgdi32 -luser32 -ladvapi32 -lssp
	x86_64-w64-mingw32-strip -o $@ build/LiDE50Scan.full.exe

app: dist/LiDE50Scan.exe
.PHONY: app

# ---- tests (run on the build machine, no Windows needed) ----
# The file encoders and the image adjustments are plain C++. The test builds them with the
# host compiler and AddressSanitizer/UBSan, writes every format and reads it back.
# (shift-base is off because stb_image_write shifts negative numbers on purpose.)
HOSTCXX ?= g++
HOSTCC ?= gcc
build/tests/run_tests: tests/test_main.cpp app/encoders.cpp app/process.cpp third_party/miniz.c app/*.h
	@mkdir -p build/tests
	$(HOSTCC) -O1 -g -c third_party/miniz.c -o build/tests/miniz.o
	$(HOSTCXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Iapp \
	  -o $@ tests/test_main.cpp app/encoders.cpp app/process.cpp build/tests/miniz.o

test: build/tests/run_tests
	build/tests/run_tests
.PHONY: test

clean:
	rm -rf build dist
.PHONY: clean

-include $(shell find build -name '*.d' 2>/dev/null)
