# madc_modules.mk — madc MODULES (include/madc_module.h): a library found by
# NAME on the module path (<dir>/<NAME>.<dso>), describing itself through one
# entry point. Its declarations are GENERATED from its face by the madc this
# tree builds (`madc --std=madc --emit-module-table=<out.inc> <face>`) and
# compiled into the module beside its own C++, which includes the table and
# returns it in its description. Nothing about a module lives in the core.
#
# The module binds libmadc's own symbols (madc::value, the registration API
# its builders call) at LOAD, from the image that imported it: undefined at
# link on ELF (bin/madc links -rdynamic).
#
# The Linux host arm only, so far: a module's darwin and win64 arms arrive with
# the first module that ships (plan 2026-10-17-dialect-builtin-registration.md,
# M2). A cross or hosted MODE builds no module here.

# The test modules (tests/modules/<name>/<name>.madi + <name>.cpp): built beside
# the object tree, mode-independent, found by the suite through
# --module-path=obj/test-modules.
TEST_MODULE_DIR = ../obj/test-modules
TEST_MODULES = greet

# One module: its generated table from its face, then the library. The
# generator is this tree's madc ($(BIN)) — the image that imports the module.
define madc_module_rules
$(2)/$(1).inc: $(3)/$(1).madi $(BIN)
	@mkdir -p $(2)
	$(BIN) --std=madc --emit-module-table=$(2)/$(1).inc.tmp $(3)/$(1).madi
	mv $(2)/$(1).inc.tmp $(2)/$(1).inc

$(2)/$(1).so: $(3)/$(1).cpp $(2)/$(1).inc $(INCDIR)/madc_module.h
	$(CXX) -shared -fPIC -o $(2)/$(1).so $(CXXFLAGS) $(DEFINES) -I. -I$(2) $(3)/$(1).cpp
endef

ifeq ($(filter cross-% hosted-%,$(MODE)),)
TEST_MODULE_LIBS = $(patsubst %,$(TEST_MODULE_DIR)/%.so,$(TEST_MODULES))
$(foreach m,$(TEST_MODULES),$(eval $(call madc_module_rules,$(m),$(TEST_MODULE_DIR),../tests/modules/$(m))))
endif

.PHONY: test-modules
test-modules: $(TEST_MODULE_LIBS)
