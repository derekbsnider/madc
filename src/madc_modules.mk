# madc_modules.mk — madc MODULES (include/madc_module.h): a library found by
# NAME on the module path (<dir>/<NAME>.<dso>), describing itself through one
# entry point. Its declarations are GENERATED from its face by madc
# (`madc --std=madc --emit-module-table=<out.inc> <face>`) and compiled into
# the module beside its own C++, which includes the table and returns it in its
# description. Nothing about a module lives in the core.
#
# The module binds libmadc's own symbols (madc::value, the registration API
# its builders call) at LOAD, from the image that imported it: undefined at
# link on ELF (bin/madc links -rdynamic) and Mach-O (-undefined
# dynamic_lookup); a PE DLL links libmadc's import lib.

# The generated tables: one per module for EVERY target (the builders name
# DataDefs, never sizes or symbols — the dialect table's property), so one
# directory, mode-independent. The generator is this mode's madc where it runs
# on the build host (MODE_RUNS_ON_HOST, src/Makefile), else the host's dev
# madc (a cross build runs after the host build, as the dialect table's R3
# carry does).
MODULE_TABLE_DIR = ../obj/modules
ifdef MODE_RUNS_ON_HOST
MODULE_TABLE_GEN = $(BIN)
else
MODULE_TABLE_GEN = ../bin/madc
endif

# $(call madc_module_table_rules,NAME,FACE): $(MODULE_TABLE_DIR)/NAME/NAME.inc
# from FACE. The stamp moves the table only when its text changed, so a madc
# relink regenerates without relinking the module; everything beside the face
# (what it includes) is a prerequisite.
define madc_module_table_rules
$(MODULE_TABLE_DIR)/$(1)/$(1).stamp: $(2) $(wildcard $(dir $(2))*) $(MODULE_TABLE_GEN)
	@mkdir -p $(MODULE_TABLE_DIR)/$(1)
	$(MODULE_TABLE_GEN) --std=madc --emit-module-table=$(MODULE_TABLE_DIR)/$(1)/$(1).inc.tmp $(2)
	if cmp -s $(MODULE_TABLE_DIR)/$(1)/$(1).inc.tmp $(MODULE_TABLE_DIR)/$(1)/$(1).inc; then rm -f $(MODULE_TABLE_DIR)/$(1)/$(1).inc.tmp; else mv $(MODULE_TABLE_DIR)/$(1)/$(1).inc.tmp $(MODULE_TABLE_DIR)/$(1)/$(1).inc; fi
	touch $(MODULE_TABLE_DIR)/$(1)/$(1).stamp
$(MODULE_TABLE_DIR)/$(1)/$(1).inc: $(MODULE_TABLE_DIR)/$(1)/$(1).stamp ;
endef

# The test modules (tests/modules/<name>/<name>.madi + <name>.cpp): built
# beside the object tree on the Linux host, found by the suite through
# --module-path=obj/test-modules.
TEST_MODULE_DIR = ../obj/test-modules
TEST_MODULES = greet

define madc_test_module_rules
$(eval $(call madc_module_table_rules,$(1),../tests/modules/$(1)/$(1).madi))
$(TEST_MODULE_DIR)/$(1).so: ../tests/modules/$(1)/$(1).cpp $(MODULE_TABLE_DIR)/$(1)/$(1).inc $(INCDIR)/madc_module.h
	@mkdir -p $(TEST_MODULE_DIR)
	$(CXX) -shared -fPIC -o $(TEST_MODULE_DIR)/$(1).so $(CXXFLAGS) $(DEFINES) -I. -I$(MODULE_TABLE_DIR)/$(1) ../tests/modules/$(1)/$(1).cpp
endef

ifeq ($(filter cross-% hosted-%,$(MODE)),)
TEST_MODULE_LIBS = $(patsubst %,$(TEST_MODULE_DIR)/%.so,$(TEST_MODULES))
$(foreach m,$(TEST_MODULES),$(eval $(call madc_test_module_rules,$(m))))
endif

.PHONY: test-modules
test-modules: $(TEST_MODULE_LIBS)
