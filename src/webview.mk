# Optional platform library: make -C src libmadcwebview (or the named cross
# targets below). It is not linked into libmadc or loaded by non-GUI programs.
WEBVIEW_DIR = ../third_party/webview
WEBVIEW_SOURCE = $(WEBVIEW_DIR)/core/src/webview.cc
# madc's own extension of the library (the native menu bar, S2): compiled
# beside upstream's source into the same shared object; its header is what
# scripts/gen_webview_header.py appends to the embedded include/madc/webview.h.
WEBVIEW_MADC_SOURCE = madcwebview_menu.cc
WEBVIEW_MADC_HEADER = madcwebview_menu.h
WEBVIEW_HEADERS = $(wildcard $(WEBVIEW_DIR)/core/include/*.h $(WEBVIEW_DIR)/core/include/webview/*.h $(WEBVIEW_DIR)/compatibility/mingw/include/*.h)
WEBVIEW_WEBKITGTK_API ?= 6.0
WEBVIEW_MACOS_MINOS ?= 13.3
WEBVIEW2_SDK_DIR ?= $(abspath ../obj/webview/webview2-sdk)
WEBVIEW_FLAGS = -O2 -DWEBVIEW_BUILD_SHARED -I$(WEBVIEW_DIR)/core/include
WEBVIEW_BUILD_DIR = ../obj/webview/$(MODE)
WEBVIEW_CXX = $(CXX)

ifeq ($(MODE),hosted-x86-64-windows)
WEBVIEW_LIBRARY = ../bin/madcwebview.dll
WEBVIEW_CXXFLAGS = -std=c++14 $(WEBVIEW_FLAGS) -I$(WEBVIEW_DIR)/compatibility/mingw/include -I$(WEBVIEW2_SDK_DIR)/build/native/include
# Use the SAME UCRT C++/pthread runtime as madc; never introduce an MSVCRT
# libstdc++ copy. win_ucrt_compat supplies the static-libgcc setjmp imports.
WEBVIEW_LDFLAGS = -shared -static-libgcc -L$(WIN_UCRT_LIBSTDCXX)/lib -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -lpthread
WEBVIEW_EXTRA = $(OBJDIR)/win_ucrt_compat.o $(WEBVIEW2_SDK_DIR)/.sha256
else ifdef HOSTED_DARWIN_TARGET
WEBVIEW_CXX = $(DARWIN_CLANGXX) -target $(subst x86-64,x86_64,$(DARWIN_ARCH))-apple-macos$(WEBVIEW_MACOS_MINOS) --sysroot $(MACOS_SDK) $(DARWIN_CXX_ISYS)
WEBVIEW_LIBRARY = ../lib/webview/$(DARWIN_ARCH)-macos/libmadcwebview.dylib
WEBVIEW_CXXFLAGS = -std=c++11 $(WEBVIEW_FLAGS)
WEBVIEW_LDFLAGS = $(DARWIN_LD_FLAGS) -dynamiclib -Wl,-install_name,@rpath/libmadcwebview.dylib -framework WebKit
else
WEBVIEW_LIBRARY = ../lib/libmadcwebview.so
WEBVIEW_CXXFLAGS = -std=c++11 $(WEBVIEW_FLAGS) -fPIC $(shell pkg-config --cflags webkitgtk-$(WEBVIEW_WEBKITGTK_API) gtk4 2>/dev/null)
WEBVIEW_LDFLAGS = -shared -Wl,-soname,libmadcwebview.so $(shell pkg-config --libs webkitgtk-$(WEBVIEW_WEBKITGTK_API) gtk4 2>/dev/null) -ldl
endif

.PHONY: libmadcwebview webview-windows webview-macos webview-arm64-macos webview-x86-64-macos webview-deps webview-header-check
libmadcwebview: $(WEBVIEW_LIBRARY)
ifeq ($(MODE),hosted-x86-64-windows)
libmadcwebview: ../bin/libstdc++-6.dll ../bin/libwinpthread-1.dll
endif
webview-windows:
	$(MAKE) MODE=hosted-x86-64-windows libmadcwebview
webview-macos: webview-arm64-macos webview-x86-64-macos
webview-arm64-macos webview-x86-64-macos:
	$(MAKE) MODE=hosted-$(patsubst webview-%,%,$@) libmadcwebview

webview-deps:
ifeq ($(MODE),hosted-x86-64-windows)
	@test -f $(WIN_UCRT_LIBSTDCXX)/lib/libstdc++.dll.a || { echo 'webview needs the UCRT stage: scripts/build_win_ucrt_libstdcxx.sh' >&2; exit 1; }
else ifndef HOSTED_DARWIN_TARGET
	@test '$(WEBVIEW_WEBKITGTK_API)' = 6.0 || { echo 'madc webview requires WEBVIEW_WEBKITGTK_API=6.0' >&2; exit 1; }
	pkg-config --exists webkitgtk-$(WEBVIEW_WEBKITGTK_API) gtk4 || { echo 'webview needs libwebkitgtk-6.0-dev (scripts/provision_container.sh)' >&2; exit 1; }
endif

$(WEBVIEW2_SDK_DIR)/.sha256: ../scripts/fetch_webview2_sdk.sh
	bash $< $(WEBVIEW2_SDK_DIR)

# A command stamp makes knob/toolchain changes rebuild this library, while
# identical commands leave the artifact alone on repeated invocations.
$(WEBVIEW_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(WEBVIEW_CXX) $(WEBVIEW_CXXFLAGS) $(WEBVIEW_LDFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(WEBVIEW_LIBRARY): $(WEBVIEW_SOURCE) $(WEBVIEW_MADC_SOURCE) $(WEBVIEW_MADC_HEADER) $(WEBVIEW_HEADERS) $(WEBVIEW_EXTRA) $(WEBVIEW_BUILD_DIR)/command | webview-deps
	mkdir -p $(dir $@)
	$(WEBVIEW_CXX) $(WEBVIEW_CXXFLAGS) -I. $(WEBVIEW_SOURCE) $(WEBVIEW_MADC_SOURCE) $(filter %.o,$(WEBVIEW_EXTRA)) -o $@ $(WEBVIEW_LDFLAGS)

webview-header-check:
	python3 ../scripts/gen_webview_header.py --check
