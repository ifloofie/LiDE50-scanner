CXX = x86_64-w64-mingw32-g++-posix
CC  = x86_64-w64-mingw32-gcc-posix
WINDRES = x86_64-w64-mingw32-windres
OPT = -O2 -g1
CXXFLAGS = $(OPT) -std=c++17 -Icore/include -Ideps/include/libusb-1.0 -DBACKEND_NAME=genesys -D_WIN32_WINNT=0x0A00 -Wno-attributes
CFLAGS = $(OPT) -Icore/include -DBACKEND_NAME=genesys

CORE_SRCS = $(wildcard core/genesys/*.cpp) core/shim/sanei_win.cpp core/shim/lide50_sim.cpp
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

APP_SRCS = app/main.cpp app/scanner_engine.cpp app/process.cpp app/writers.cpp app/winusb_setup.cpp app/log.cpp app/ui/gfx.cpp app/ui/ui.cpp
APP_OBJS = $(patsubst %.cpp,build/%.o,$(APP_SRCS)) build/third_party/miniz.o build/app/app_res.o
APP_CXXFLAGS = $(OPT) -std=c++17 -Icore/include -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wno-unknown-pragmas

build/app/%.o: app/%.cpp app/*.h app/ui/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(APP_CXXFLAGS) -c $< -o $@

build/third_party/miniz.o: third_party/miniz.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -c $< -o $@

build/app/app_res.o: app/app.rc app/app.ico app/app.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) -i app/app.rc -o $@ --include-dir app

dist/LiDE50Scan.exe: $(APP_OBJS) build/libgenesys_lide50.a deps/lib/libusb-1.0.a
	@mkdir -p dist
	$(CXX) -municode -mwindows -static -Wl,--stack,8388608 -Wl,-Map,build/LiDE50Scan.map -o build/LiDE50Scan.full.exe $(APP_OBJS) build/libgenesys_lide50.a deps/lib/libusb-1.0.a \
	  -ld2d1 -ldwrite -ldwmapi -lsetupapi -lcfgmgr32 -lnewdev -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lmsimg32 -lgdi32 -luser32 -ladvapi32
	x86_64-w64-mingw32-strip -o $@ build/LiDE50Scan.full.exe

app: dist/LiDE50Scan.exe
.PHONY: app
