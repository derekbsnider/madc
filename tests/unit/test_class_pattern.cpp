#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if (madc_verbose) { x; } } while (0)

#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>
#include <unistd.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

static std::string fixture_path()
{
	const char *paths[] = {
		"../tests/testclasspatternbasic.mad",
		"tests/testclasspatternbasic.mad"
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
		if (access(paths[i], R_OK) == 0)
			return paths[i];
	return paths[0];
}

static std::shared_ptr<Program> parse_fixture(bool force_legacy, bool &ok)
{
	std::shared_ptr<Program> program = std::make_shared<Program>();
	program->force_legacy_class_patterns = force_legacy;
	std::string path = fixture_path();
	TokenProgram *root = program->tokenize(path.c_str());
	ok = root && program->parse(root);
	return program;
}

static DataDefCLASS *find_canonical_class(Program &program,
	const std::string &canonical)
{
	for (datadef_map_citer it = program.struct_map.begin();
	     it != program.struct_map.end(); ++it) {
		DataDefCLASS *candidate = dynamic_cast<DataDefCLASS *>(it->second);
		if (candidate && candidate->canonical_cpp_spelling() == canonical)
			return candidate;
	}
	return NULL;
}

static std::string type_shape(DataDef *type)
{
	if (!type)
		return "<null>";
	if (DataDefCONST *qualified = dynamic_cast<DataDefCONST *>(type))
		return "const(" + type_shape(qualified->base_type) + ")";
	if (DataDefREF *reference = dynamic_cast<DataDefREF *>(type))
		return "ref(" + type_shape(reference->base_type) + ")";
	if (DataDefPTR *pointer = dynamic_cast<DataDefPTR *>(type))
		return "ptr(" + type_shape(pointer->base_type) + ")";
	if (DataDefCArray *array = dynamic_cast<DataDefCArray *>(type))
		return "array[" + std::to_string(array->count) + "](" +
		       type_shape(array->element_type) + ")";
	std::string spelling = type->canonical_cpp_spelling().empty()
		? type->name : type->canonical_cpp_spelling();
	return spelling + ":" + std::to_string(type->size)
	     + ":" + std::to_string((uint32_t)type->rawtype());
}

static void check_method_equivalent(DataDefCLASS *pattern_owner,
	DataDefCLASS *legacy_owner, const std::string &name)
{
	Variable *pattern_var = pattern_owner->findMethod(name);
	Variable *legacy_var = legacy_owner->findMethod(name);
	REQUIRE(pattern_var != NULL);
	REQUIRE(legacy_var != NULL);
	CHECK(pattern_var->name == legacy_var->name);
	CHECK(pattern_var->flags == legacy_var->flags);
	CHECK(pattern_var->storage_alias_name == legacy_var->storage_alias_name);
	FuncDef *pattern_fn = dynamic_cast<FuncDef *>(pattern_var->type);
	FuncDef *legacy_fn = dynamic_cast<FuncDef *>(legacy_var->type);
	REQUIRE(pattern_fn != NULL);
	REQUIRE(legacy_fn != NULL);
	CHECK(type_shape(&pattern_fn->returns) == type_shape(&legacy_fn->returns));
	CHECK(pattern_fn->declaration_only == legacy_fn->declaration_only);
	CHECK(pattern_fn->is_const_method == legacy_fn->is_const_method);
	CHECK(pattern_fn->is_void_params == legacy_fn->is_void_params);
	CHECK(pattern_fn->method_display_name == legacy_fn->method_display_name);
	CHECK(pattern_fn->return_typedef_name == legacy_fn->return_typedef_name);
	CHECK(pattern_fn->emit_symbol == legacy_fn->emit_symbol);
	CHECK(pattern_fn->param_cpp_spellings == legacy_fn->param_cpp_spellings);
	CHECK(pattern_fn->param_typedef_names == legacy_fn->param_typedef_names);
	CHECK(pattern_fn->const_params == legacy_fn->const_params);
	REQUIRE(pattern_fn->parameters.size() == legacy_fn->parameters.size());
	for (size_t i = 0; i < pattern_fn->parameters.size(); ++i)
		CHECK(type_shape(pattern_fn->parameters[i]) ==
		      type_shape(legacy_fn->parameters[i]));
	REQUIRE(pattern_var->data != NULL);
	REQUIRE(legacy_var->data != NULL);
	Method *pattern_method = static_cast<Method *>(pattern_var->data);
	Method *legacy_method = static_cast<Method *>(legacy_var->data);
	CHECK(pattern_method->owner_class == pattern_owner);
	CHECK(legacy_method->owner_class == legacy_owner);
	CHECK(pattern_method->parameters.size() == legacy_method->parameters.size());
}

static void check_class_equivalent(DataDefCLASS *pattern,
	DataDefCLASS *legacy)
{
	REQUIRE(pattern != NULL);
	REQUIRE(legacy != NULL);
	CHECK(pattern->is_complete);
	CHECK(legacy->is_complete);
	CHECK(pattern->canonical_cpp_spelling() == legacy->canonical_cpp_spelling());
	CHECK(pattern->size == legacy->size);
	CHECK(pattern->alignment() == legacy->alignment());
	CHECK(pattern->nvsize == legacy->nvsize);
	CHECK(pattern->own_block_off == legacy->own_block_off);
	CHECK(pattern->member_counts == legacy->member_counts);
	CHECK(pattern->member_array_flags == legacy->member_array_flags);
	CHECK(pattern->member_offsets == legacy->member_offsets);
	CHECK(pattern->member_dims == legacy->member_dims);
	CHECK(pattern->member_access == legacy->member_access);
	CHECK(pattern->member_origin == legacy->member_origin);
	REQUIRE(pattern->members.size() == legacy->members.size());
	for (size_t i = 0; i < pattern->members.size(); ++i) {
		CHECK(pattern->members[i].first == legacy->members[i].first);
		CHECK(type_shape(pattern->members[i].second) ==
		      type_shape(legacy->members[i].second));
	}
	CHECK(pattern->methods.size() == legacy->methods.size());
}

TEST_CASE("B2 basic ClassPattern substitution matches the sole parser lane")
{
	bool pattern_ok = false;
	bool legacy_ok = false;
	std::shared_ptr<Program> pattern = parse_fixture(false, pattern_ok);
	std::shared_ptr<Program> legacy = parse_fixture(true, legacy_ok);
	REQUIRE(pattern_ok);
	REQUIRE(legacy_ok);
	CHECK(pattern->_class_inst_pattern == 3);
	CHECK(pattern->_class_inst_parse == 0);
	CHECK(pattern->_class_inst_opaque == 1);
	CHECK(legacy->_class_inst_pattern == 0);
	CHECK(legacy->_class_inst_parse == 3);
	CHECK(legacy->_class_inst_opaque == 1);

	DataDefCLASS *pattern_i64 = find_canonical_class(
		*pattern, "PatternBasic<int64_t>");
	DataDefCLASS *legacy_i64 = find_canonical_class(
		*legacy, "PatternBasic<int64_t>");
	DataDefCLASS *pattern_char = find_canonical_class(
		*pattern, "PatternBasic<char>");
	DataDefCLASS *legacy_char = find_canonical_class(
		*legacy, "PatternBasic<char>");
	DataDefCLASS *pattern_forward = find_canonical_class(
		*pattern, "PatternForward<int64_t>");
	DataDefCLASS *legacy_forward = find_canonical_class(
		*legacy, "PatternForward<int64_t>");
	check_class_equivalent(pattern_i64, legacy_i64);
	check_class_equivalent(pattern_char, legacy_char);
	check_class_equivalent(pattern_forward, legacy_forward);

	REQUIRE(pattern_i64->type_aliases.count("value_type") == 1);
	REQUIRE(legacy_i64->type_aliases.count("value_type") == 1);
	CHECK(type_shape(pattern_i64->type_aliases["value_type"]) ==
	      type_shape(legacy_i64->type_aliases["value_type"]));
	REQUIRE(pattern_i64->type_aliases.count("pointer") == 1);
	REQUIRE(legacy_i64->type_aliases.count("pointer") == 1);
	CHECK(type_shape(pattern_i64->type_aliases["pointer"]) ==
	      type_shape(legacy_i64->type_aliases["pointer"]));
	check_method_equivalent(pattern_i64, legacy_i64, "read");
	check_method_equivalent(pattern_i64, legacy_i64, "choose");
	FuncDef *pattern_read = dynamic_cast<FuncDef *>(
		pattern_i64->findMethod("read")->type);
	FuncDef *pattern_choose = dynamic_cast<FuncDef *>(
		pattern_i64->findMethod("choose")->type);
	REQUIRE(pattern_read != NULL);
	REQUIRE(pattern_choose != NULL);
	CHECK(pattern_read->emit_symbol ==
	      "_ZNK12PatternBasicIlE4readEPl");
	CHECK(pattern_choose->emit_symbol ==
	      "_ZN12PatternBasicIlE6chooseEl");

	Variable *pattern_before = pattern->findVariable("forward_before");
	Variable *legacy_before = legacy->findVariable("forward_before");
	REQUIRE(pattern_before != NULL);
	REQUIRE(legacy_before != NULL);
	DataDefPTR *pattern_pointer = dynamic_cast<DataDefPTR *>(pattern_before->type);
	DataDefPTR *legacy_pointer = dynamic_cast<DataDefPTR *>(legacy_before->type);
	REQUIRE(pattern_pointer != NULL);
	REQUIRE(legacy_pointer != NULL);
	CHECK(pattern_pointer->base_type == pattern_forward);
	CHECK(legacy_pointer->base_type == legacy_forward);
	CHECK(pattern_i64 != pattern_char);
}

TEST_CASE("B2 admitted ClassPattern failures roll back without parser retry")
{
	const char *definition =
		"template<typename T> class PatternFailure {\n"
		"public:\n"
		"    T value;\n"
		"    T read(T *p) const;\n"
		"};\n";
	const char *complete_source =
		"template<typename T> class PatternFailure {\n"
		"public:\n"
		"    T value;\n"
		"    T read(T *p) const;\n"
		"};\n"
		"PatternFailure<int64_t> probe;\n";

	Program oracle;
	TokenProgram *oracle_tokens = oracle.tokenize_buffer(
		complete_source, "<class-pattern-failure-oracle>");
	REQUIRE(oracle_tokens != NULL);
	REQUIRE(oracle.parse(oracle_tokens));
	DataDefCLASS *oracle_class = find_canonical_class(
		oracle, "PatternFailure<int64_t>");
	REQUIRE(oracle_class != NULL);
	std::string registered_name = oracle_class->name;
	std::string collision_name = registered_name + "__read";

	Program program;
	TokenProgram *definition_tokens = program.tokenize_buffer(
		definition, "<class-pattern-failure-definition>");
	REQUIRE(definition_tokens != NULL);
	REQUIRE(program.parse(definition_tokens));
	Program::TemplateDef *template_def = program.find_template(
		"PatternFailure");
	REQUIRE(template_def != NULL);
	REQUIRE(template_def->class_pattern_id != 0);
	Program::ClassPattern *captured_pattern =
		program.class_pattern_arena.get(template_def->class_pattern_id);
	REQUIRE(captured_pattern != NULL);

	FuncDef *collision = new FuncDef(ddINT);
	program.funcdef_map[collision_name] = collision;
	TokenProgram *use_tokens = program.tokenize_buffer(
		"PatternFailure<int64_t> failed;\n",
		"<class-pattern-failure-use>");
	REQUIRE(use_tokens != NULL);
	CHECK_FALSE(program.parse(use_tokens));
	CHECK(program._class_inst_pattern == 1);
	CHECK(program._class_inst_parse == 0);
	REQUIRE(program.funcdef_map.count(collision_name) == 1);
	CHECK(program.funcdef_map.find(collision_name)->second == collision);
	CHECK(program.struct_map.find(registered_name) ==
	      program.struct_map.end());
	CHECK(program.datatype_map.find(registered_name) ==
	      program.datatype_map.end());
}
