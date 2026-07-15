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

static std::string fixture_path(const char *fixture)
{
	const char *prefixes[] = {
		"../tests/",
		"tests/"
	};
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
		std::string path = std::string(prefixes[i]) + fixture;
		if (access(path.c_str(), R_OK) == 0)
			return path;
	}
	return std::string(prefixes[0]) + fixture;
}

static std::shared_ptr<Program> parse_fixture(bool force_legacy, bool &ok,
	const char *fixture = "testclasspatternbasic.mad")
{
	std::shared_ptr<Program> program = std::make_shared<Program>();
	program->force_legacy_class_patterns = force_legacy;
	std::string path = fixture_path(fixture);
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

static std::string method_shape(Variable *var)
{
	if (!var)
		return "<null>";
	FuncDef *fd = dynamic_cast<FuncDef *>(var->type);
	if (!fd)
		return var->name + ":<not-function>";
	std::string out = var->name + "|" + fd->method_display_name + "|"
		+ fd->emit_symbol + "|" + fd->local_emit_name + "|"
		+ type_shape(&fd->returns) + "|"
		+ std::to_string(var->flags) + "|"
		+ std::to_string(fd->declaration_only) + "|"
		+ std::to_string(fd->is_const_method) + "|"
		+ std::to_string(fd->is_member_template);
	for (size_t i = 0; i < fd->parameters.size(); ++i) {
		out += "|p:" + type_shape(fd->parameters[i]);
		out += i < fd->param_cpp_spellings.size()
			? ":" + fd->param_cpp_spellings[i] : ":<no-spelling>";
		out += i < fd->param_typedef_names.size()
			? ":" + fd->param_typedef_names[i] : ":<no-alias>";
	}
	return out;
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
	REQUIRE(pattern->bases.size() == legacy->bases.size());
	for (size_t i = 0; i < pattern->bases.size(); ++i) {
		CHECK(type_shape(pattern->bases[i].base) ==
		      type_shape(legacy->bases[i].base));
		CHECK(pattern->bases[i].offset == legacy->bases[i].offset);
		CHECK(pattern->bases[i].is_virtual == legacy->bases[i].is_virtual);
		CHECK(pattern->bases[i].access == legacy->bases[i].access);
		CHECK(pattern->bases[i].is_primary == legacy->bases[i].is_primary);
	}
	REQUIRE(pattern->members.size() == legacy->members.size());
	for (size_t i = 0; i < pattern->members.size(); ++i) {
		CHECK(pattern->members[i].first == legacy->members[i].first);
		CHECK(type_shape(pattern->members[i].second) ==
		      type_shape(legacy->members[i].second));
	}
	REQUIRE(pattern->methods.size() == legacy->methods.size());
	for (size_t i = 0; i < pattern->methods.size(); ++i)
		CHECK(method_shape(pattern->methods[i]) ==
		      method_shape(legacy->methods[i]));
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

TEST_CASE("B3 structural registration transactions restore first writes")
{
	registration_map<std::string, int> values;
	values["existing"] = 1;
	registration_map<std::string, int>::transaction_state value_transaction;
	values.begin_transaction(value_transaction);
	values["existing"] = 2;
	values["existing"] = 3;
	values["created"] = 4;
	values.erase("existing");
	values.rollback_transaction(value_transaction);
	REQUIRE(values.size() == 1);
	CHECK(values["existing"] == 1);
	CHECK(values.count("created") == 0);

	values.begin_transaction(value_transaction);
	values["existing"] = 5;
	values["committed"] = 6;
	values.commit_transaction(value_transaction);
	CHECK(values["existing"] == 5);
	CHECK(values["committed"] == 6);

	FuncDef original_func(ddINT32);
	FuncDef replacement_func(ddINT64);
	Variable original_var("original", ddINT32, 1, NULL, false);
	Variable replacement_var("replacement", ddINT64, 1, NULL, false);
	Program program;
	program.funcdef_map["existing"] = &original_func;
	program.namespace_map["known"]["existing"] = &original_var;
	program.struct_map.set("existing", &ddINT32);
	program.deferred_lazy_bodies["existing"].line = 7;
	program.out_of_line_member_defs["existing"].push_back(
		Program::OutOfLineMemberDef());
	program.forest_arena_enabled = true;
	{
		Program::ClassRegistrationJournal journal(program);
		CHECK(program.class_registration_taps_muted);
		CHECK_FALSE(program.forest_arena_enabled);
		program.funcdef_map["existing"] = &replacement_func;
		program.funcdef_map["created"] = &replacement_func;
		program.namespace_map["known"]["existing"] = &replacement_var;
		program.namespace_map["known"]["created"] = &replacement_var;
		program.namespace_map["created"]["entry"] = &replacement_var;
		program.struct_map.set("existing", &ddINT64);
		program.struct_map.set("created", &ddINT64);
		program.deferred_lazy_bodies["existing"].line = 9;
		program.deferred_lazy_bodies["created"].line = 11;
		program.out_of_line_member_defs["existing"].push_back(
			Program::OutOfLineMemberDef());
		program.out_of_line_member_defs["created"].push_back(
			Program::OutOfLineMemberDef());
		journal.rollback();
	}
	CHECK(program.funcdef_map["existing"] == &original_func);
	CHECK(program.funcdef_map.count("created") == 0);
	CHECK(program.namespace_map["known"]["existing"] == &original_var);
	CHECK(program.namespace_map["known"].count("created") == 0);
	CHECK(program.namespace_map.count("created") == 0);
	CHECK(program.struct_map.find("existing")->second == &ddINT32);
	CHECK(program.struct_map.find("created") == program.struct_map.end());
	CHECK(program.deferred_lazy_bodies["existing"].line == 7);
	CHECK(program.deferred_lazy_bodies.count("created") == 0);
	REQUIRE(program.out_of_line_member_defs["existing"].size() == 1);
	CHECK(program.out_of_line_member_defs.count("created") == 0);
	CHECK_FALSE(program.class_registration_taps_muted);
	CHECK(program.forest_arena_enabled);
}

TEST_CASE("B3 capture indexes nested templates by their parsed owner")
{
	const char *source =
		"template<class T> struct PatternNested {\n"
		"    template<class U> struct Rebind { U value; };\n"
		"    template<class U> using Pointer = U *;\n"
		"    T value;\n"
		"};\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-nested-templates>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));
	Program::TemplateDef *definition = program.find_template("PatternNested");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *pattern =
		program.class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(pattern != NULL);
	REQUIRE(pattern->nodes.size() == 1);
	REQUIRE(pattern->nodes[0].nested_templates.size() == 2);
	bool saw_class = false;
	bool saw_alias = false;
	for (size_t i = 0; i < pattern->nodes[0].nested_templates.size(); ++i) {
		const Program::ClassNestedTemplatePattern &nested =
			pattern->nodes[0].nested_templates[i];
		saw_class = saw_class
			|| (nested.kind == Program::ClassNestedTemplateKind::ClassTemplate
			    && nested.class_name == "Rebind" && !nested.body.empty());
		saw_alias = saw_alias
			|| (nested.kind == Program::ClassNestedTemplateKind::AliasTemplate
			    && nested.class_name == "Pointer" && !nested.target.empty());
	}
	CHECK(saw_class);
	CHECK(saw_alias);
}

TEST_CASE("B3 dependent non-type template arguments stay on the fallback lane")
{
	const char *source =
		"template<class T> struct PatternTrait {\n"
		"    static const bool value = true;\n"
		"};\n"
		"template<class T, bool Enabled = PatternTrait<T>::value> "
		"struct PatternSelect {\n"
		"    using type = T;\n"
		"};\n"
		"template<class T> struct PatternValueArg {\n"
		"    using selected = typename PatternSelect<T>::type;\n"
		"    selected value;\n"
		"};\n"
		"template<class T> struct PatternValueOuter {\n"
		"    using selected = typename PatternValueArg<T>::selected;\n"
		"    selected value;\n"
		"};\n"
		"PatternValueOuter<int64_t> probe;\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-dependent-value>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));
	Program::TemplateDef *definition = program.find_template(
		"PatternValueArg");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *pattern =
		program.class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(pattern != NULL);
	CHECK(pattern->capture_reason ==
	      Program::ClassParseReason::DependentValueExpression);
	Program::TemplateDef *outer_definition = program.find_template(
		"PatternValueOuter");
	REQUIRE(outer_definition != NULL);
	REQUIRE(outer_definition->class_pattern_id != 0);
	const Program::ClassPattern *outer_pattern =
		program.class_pattern_arena.get(outer_definition->class_pattern_id);
	REQUIRE(outer_pattern != NULL);
	CHECK(outer_pattern->capture_reason ==
	      Program::ClassParseReason::DependentValueExpression);
	CHECK(program._class_inst_parse >= 1);
}

TEST_CASE("B3 dependent member types require a guaranteed pattern member")
{
	const char *source =
		"template<class T> struct PatternMaybe { };\n"
		"template<class T> struct PatternMaybe<T *> {\n"
		"    using value_type = T;\n"
		"};\n"
		"template<class T> struct PatternMaybeUse {\n"
		"    using value_type = typename PatternMaybe<T>::value_type;\n"
		"};\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-dependent-member>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));
	Program::TemplateDef *definition = program.find_template(
		"PatternMaybeUse");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *pattern =
		program.class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(pattern != NULL);
	CHECK(pattern->capture_reason ==
	      Program::ClassParseReason::UnnormalizableType);
}

TEST_CASE("B3 vector closure matches the sole parser lane and compiler ABI")
{
	bool pattern_ok = false;
	bool legacy_ok = false;
	std::shared_ptr<Program> pattern = parse_fixture(false, pattern_ok,
		"testclasspatternvector.mad");
	std::shared_ptr<Program> legacy = parse_fixture(true, legacy_ok,
		"testclasspatternvector.mad");
	REQUIRE(pattern_ok);
	REQUIRE(legacy_ok);
	CHECK(pattern->_class_inst_pattern == 6);
	CHECK(pattern->_class_inst_parse == 0);
	CHECK(pattern->_class_inst_cache == 0);
	CHECK(pattern->_class_inst_opaque == 0);
	CHECK(legacy->_class_inst_pattern == 0);
	CHECK(legacy->_class_inst_parse == 6);
	CHECK(legacy->_class_inst_cache == 2);
	CHECK(legacy->_class_inst_opaque == 0);

	DataDefCLASS *pattern_int = find_canonical_class(
		*pattern, "PatternVectorClosure<int32_t>");
	DataDefCLASS *legacy_int = find_canonical_class(
		*legacy, "PatternVectorClosure<int32_t>");
	DataDefCLASS *pattern_long = find_canonical_class(
		*pattern, "PatternVectorClosure<int64_t>");
	DataDefCLASS *legacy_long = find_canonical_class(
		*legacy, "PatternVectorClosure<int64_t>");
	check_class_equivalent(pattern_int, legacy_int);
	check_class_equivalent(pattern_long, legacy_long);
	CHECK(pattern_int->size == 32);
	CHECK(pattern_int->alignment() == 8);
	CHECK(pattern_long->size == 48);
	CHECK(pattern_long->alignment() == 8);
	REQUIRE(pattern_int->bases.size() == 2);
	CHECK(pattern_int->bases[0].offset == 0);
	CHECK(pattern_int->bases[1].offset == 4);
	REQUIRE(pattern_long->bases.size() == 2);
	CHECK(pattern_long->bases[0].offset == 0);
	CHECK(pattern_long->bases[1].offset == 8);

	const char *nested_names[] = { "Buffer", "ImplData", "Impl" };
	for (size_t i = 0; i < sizeof(nested_names) / sizeof(nested_names[0]); ++i) {
		REQUIRE(pattern_int->type_aliases.count(nested_names[i]) == 1);
		REQUIRE(legacy_int->type_aliases.count(nested_names[i]) == 1);
		DataDefCLASS *pattern_nested = dynamic_cast<DataDefCLASS *>(
			pattern_int->type_aliases[nested_names[i]]);
		DataDefCLASS *legacy_nested = dynamic_cast<DataDefCLASS *>(
			legacy_int->type_aliases[nested_names[i]]);
		check_class_equivalent(pattern_nested, legacy_nested);
	}

	check_method_equivalent(pattern_int, legacy_int, "direct");
	check_method_equivalent(pattern_int, legacy_int, "store");
	check_method_equivalent(pattern_int, legacy_int, "operator[]");
	FuncDef *pattern_direct = dynamic_cast<FuncDef *>(
		pattern_int->findMethod("direct")->type);
	FuncDef *legacy_direct = dynamic_cast<FuncDef *>(
		legacy_int->findMethod("direct")->type);
	FuncDef *pattern_store = dynamic_cast<FuncDef *>(
		pattern_int->findMethod("store")->type);
	FuncDef *legacy_store = dynamic_cast<FuncDef *>(
		legacy_int->findMethod("store")->type);
	REQUIRE(pattern_direct != NULL);
	REQUIRE(legacy_direct != NULL);
	REQUIRE(pattern_store != NULL);
	REQUIRE(legacy_store != NULL);
	CHECK(pattern_direct->param_typedef_names.back() == "PatternElement");
	CHECK(legacy_direct->param_typedef_names.back() == "PatternElement");
	CHECK(pattern_store->param_typedef_names.back() ==
	      legacy_store->param_typedef_names.back());
	CHECK_FALSE(pattern_direct->declaration_only);
	CHECK_FALSE(legacy_direct->declaration_only);
	CHECK_FALSE(pattern_store->declaration_only);
	CHECK_FALSE(legacy_store->declaration_only);

	Program::TemplateDef *definition = pattern->find_template(
		"PatternVectorClosure");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *captured =
		pattern->class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(captured != NULL);
	CHECK(captured->capture_reason == Program::ClassParseReason::None);
	REQUIRE(captured->nodes.size() == 4);
	REQUIRE(captured->nodes[0].bases.size() == 2);
	bool saw_ctor = false;
	bool saw_dtor = false;
	bool saw_operator = false;
	bool saw_member_template = false;
	bool saw_direct_provenance = false;
	bool saw_alias_provenance = false;
	for (size_t i = 0; i < captured->nodes[0].methods.size(); ++i) {
		const Program::ClassMethodPattern &method =
			captured->nodes[0].methods[i];
		saw_ctor = saw_ctor
			|| method.kind == Program::ClassMethodKind::Constructor;
		saw_dtor = saw_dtor
			|| method.kind == Program::ClassMethodKind::Destructor;
		saw_operator = saw_operator
			|| method.display_name == "operator[]";
		saw_member_template = saw_member_template || method.is_member_template;
		if (method.display_name == "direct" && !method.parameters.empty())
			saw_direct_provenance =
				method.parameters.back().template_param_spelled_directly;
		if (method.display_name == "store" && !method.parameters.empty())
			saw_alias_provenance =
				!method.parameters.back().template_param_spelled_directly;
	}
	CHECK(saw_ctor);
	CHECK(saw_dtor);
	CHECK(saw_operator);
	CHECK(saw_member_template);
	CHECK(saw_direct_provenance);
	CHECK(saw_alias_provenance);
}
