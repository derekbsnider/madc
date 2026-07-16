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
	const Program::TemplateDef *template_def = program.find_template(
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

TEST_CASE("B3 ClassPattern products retain their definition-file origin")
{
	Program program;
	program.forest_arena_enabled = true;
	TokenProgram *header_tokens = program.tokenize_buffer(
		"template<class T> struct PatternOrigin { T value; };\n",
		"<class-pattern-origin-header>");
	REQUIRE(header_tokens != NULL);
	REQUIRE(program.parse(header_tokens));

	TokenProgram *root_tokens = program.tokenize_buffer(
		"PatternOrigin<int32_t> probe;\n",
		"<class-pattern-origin-root>");
	REQUIRE(root_tokens != NULL);
	REQUIRE(program.parse(root_tokens));
	CHECK(program._class_inst_pattern == 1);

	DataDefCLASS *product = find_canonical_class(
		program, "PatternOrigin<int32_t>");
	REQUIRE(product != NULL);
	CHECK(product->definition_origin == AggregateDefinitionOrigin::Included);
	madc::dis::defrec record;
	REQUIRE(program.forest_arena.get_def_at(product->type_id, record));
	CHECK((record.flags & madc::dis::DF_TU_ROOT_ORIGIN) == 0);

	record.flags |= madc::dis::DF_TU_ROOT_ORIGIN;
	program.forest_arena.set_def_at(product->type_id, record);
	program.forest_arena_record_aggregate(product);
	REQUIRE(program.forest_arena.get_def_at(product->type_id, record));
	CHECK((record.flags & madc::dis::DF_TU_ROOT_ORIGIN) == 0);
}

TEST_CASE("B3 ClassPattern default-type products retain definition origin")
{
	Program program;
	program.forest_arena_enabled = true;
	TokenProgram *header_tokens = program.tokenize_buffer(
		"template<class T> struct PatternDep { T value; };\n"
		"template<class T, class C = PatternDep<T>> "
		"struct PatternOwner { C value; };\n",
		"<class-pattern-default-origin-header>");
	REQUIRE(header_tokens != NULL);
	REQUIRE(program.parse(header_tokens));

	TokenProgram *root_tokens = program.tokenize_buffer(
		"PatternOwner<int32_t> owner;\n",
		"<class-pattern-default-origin-root>");
	REQUIRE(root_tokens != NULL);
	REQUIRE(program.parse(root_tokens));

	DataDefCLASS *dependency = find_canonical_class(
		program, "PatternDep<int32_t>");
	DataDefCLASS *owner = find_canonical_class(
		program, "PatternOwner<int32_t,PatternDep<int32_t>>");
	REQUIRE(dependency != NULL);
	REQUIRE(owner != NULL);
	CHECK(dependency->definition_origin ==
	      AggregateDefinitionOrigin::Included);
	CHECK(owner->definition_origin == AggregateDefinitionOrigin::Included);

	madc::dis::defrec record;
	REQUIRE(program.forest_arena.get_def_at(dependency->type_id, record));
	CHECK((record.flags & madc::dis::DF_TU_ROOT_ORIGIN) == 0);
	REQUIRE(program.forest_arena.get_def_at(owner->type_id, record));
	CHECK((record.flags & madc::dis::DF_TU_ROOT_ORIGIN) == 0);
}

TEST_CASE("B3 ClassPattern root products retain root origin")
{
	Program program;
	program.forest_arena_enabled = true;
	TokenProgram *tokens = program.tokenize_buffer(
		"template<class T> struct PatternRoot { T value; };\n"
		"PatternRoot<int32_t> root;\n",
		"<class-pattern-root-origin>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));

	DataDefCLASS *product = find_canonical_class(
		program, "PatternRoot<int32_t>");
	REQUIRE(product != NULL);
	CHECK(product->definition_origin ==
	      AggregateDefinitionOrigin::TranslationUnitRoot);
	madc::dis::defrec record;
	REQUIRE(program.forest_arena.get_def_at(product->type_id, record));
	CHECK((record.flags & madc::dis::DF_TU_ROOT_ORIGIN) != 0);
}

TEST_CASE("B3 live included templates capture lazily at first instantiation")
{
	Program program;
	program.class_pattern_live_capture = true;
	TokenProgram *header_tokens = program.tokenize_buffer(
		"template<class T> struct PatternIncluded { T value; };\n",
		"<class-pattern-included-header>");
	REQUIRE(header_tokens != NULL);
	program.forest_root_file = "<class-pattern-included-root>";
	REQUIRE(program.parse(header_tokens));

	// Definition parse does NOT capture (no eager tax on templates that
	// are never instantiated) — capture defers to the first concrete use.
	const Program::TemplateDef *definition = program.find_template(
		"PatternIncluded");
	REQUIRE(definition != NULL);
	CHECK(definition->class_pattern_id == 0);
	CHECK(definition->class_pattern_reason ==
	      Program::ClassParseReason::None);
	CHECK(definition->class_pattern_capture_deferred);

	TokenProgram *root_tokens = program.tokenize_buffer(
		"PatternIncluded<int32_t> value;\n",
		"<class-pattern-included-root>");
	REQUIRE(root_tokens != NULL);
	REQUIRE(program.parse(root_tokens));
	// FIRST demand stays on the parse lane — it only arms the demand
	// counter (a single-use template must not pay a capture parse).
	CHECK(program._class_inst_pattern == 0);
	CHECK(program._class_inst_parse == 1);
	definition = program.find_template("PatternIncluded");
	REQUIRE(definition != NULL);
	CHECK(definition->class_pattern_id == 0);
	CHECK(definition->class_pattern_capture_deferred);
	CHECK(definition->class_pattern_use_count == 1);

	// SECOND demand (a different specialization) pays the one-time
	// capture and instantiates through the pattern lane; the outcome
	// persists on the registered definition.
	TokenProgram *more_tokens = program.tokenize_buffer(
		"PatternIncluded<int64_t> other;\n",
		"<class-pattern-included-root>");
	REQUIRE(more_tokens != NULL);
	REQUIRE(program.parse(more_tokens));
	CHECK(program._class_inst_pattern == 1);
	CHECK(program._class_inst_parse == 1);
	definition = program.find_template("PatternIncluded");
	REQUIRE(definition != NULL);
	CHECK(definition->class_pattern_id != 0);
	CHECK(!definition->class_pattern_capture_deferred);
}

TEST_CASE("B3 ClassPattern arena references survive recursive materialization")
{
	Program::ClassPatternArena arena;
	Program::ClassPattern outer;
	outer.identity = "outer";
	Program::ClassPatternId outer_id = arena.add(outer);
	const Program::ClassPattern *outer_ref = arena.get(outer_id);
	REQUIRE(outer_ref != NULL);

	for (size_t i = 0; i < 1024; ++i) {
		Program::ClassPattern nested;
		nested.identity = "nested-" + std::to_string(i);
		arena.add(std::move(nested));
	}
	CHECK(arena.get(outer_id) == outer_ref);
	CHECK(outer_ref->identity == "outer");
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

	registration_set<std::string> names;
	names.insert("existing");
	registration_set<std::string>::transaction_state name_transaction;
	names.begin_transaction(name_transaction);
	names.erase("existing");
	names.insert("existing");
	names.insert("created");
	names.erase("created");
	names.emplace("created");
	names.rollback_transaction(name_transaction);
	CHECK(names.count("existing") == 1);
	CHECK(names.count("created") == 0);
	names.begin_transaction(name_transaction);
	names.erase("existing");
	names.insert("committed");
	names.commit_transaction(name_transaction);
	CHECK(names.count("existing") == 0);
	CHECK(names.count("committed") == 1);
	names.begin_transaction(name_transaction);
	names.clear();
	names.emplace_hint(names.end(), "temporary");
	names.rollback_transaction(name_transaction);
	CHECK(names.count("committed") == 1);
	CHECK(names.count("temporary") == 0);
	registration_set<std::string> other_names;
	other_names.insert("other");
	registration_set<std::string>::transaction_state other_transaction;
	names.begin_transaction(name_transaction);
	other_names.begin_transaction(other_transaction);
	names.swap(other_names);
	names.rollback_transaction(name_transaction);
	other_names.rollback_transaction(other_transaction);
	CHECK(names.count("committed") == 1);
	CHECK(other_names.count("other") == 1);

	FuncDef original_func(ddINT32);
	FuncDef replacement_func(ddINT64);
	Variable original_var("original", ddINT32, 1, NULL, false);
	Variable replacement_var("replacement", ddINT64, 1, NULL, false);
	DataDefCLASS alias_owner("AliasOwner", 0, DataType::dtRESERVED);
	alias_owner.type_aliases["existing"] = &ddINT32;
	DataDef project_original("ProjectOriginal", 4, DataType::dtINT32);
	DataDef project_replacement("ProjectReplacement", 4, DataType::dtINT32);
	DataDef project_appended("ProjectAppended", 4, DataType::dtINT32);
	DataDef project_appended_replacement(
		"ProjectAppendedReplacement", 4, DataType::dtINT32);
	Program program;
	uint32_t project_original_id = program.type_id_for(&project_original);
	TokenSemi ast_before;
	TokenSemi ast_during;
	program.ast.push_back(&ast_before);
	program.intern_template_param("Before", 0);
	Program::PackDeclEntry pack_entry = {};
	pack_entry.name = "before";
	program.pack_decls.push_back(pack_entry);
	Program::PackDeclFrame pack_frame = {};
	pack_frame.names.push_back(std::make_pair("before", Program::pdkStruct));
	program.pack_decl_stack.push_back(pack_frame);
	program.block_typedef_shadows.push_back(
		std::vector<std::pair<std::string, TokenDataType *> >());
	program.block_typedef_shadows.back().push_back(
		std::make_pair("before", (TokenDataType *)NULL));
	Program::ConceptDef original_concept;
	original_concept.defining_namespace = "original";
	program.concept_map["existing"] = original_concept;
	HoistedDeclIdentity original_identity;
	original_identity.symbol = "original";
	program.function_local_class_identities[&alias_owner] = original_identity;
	program.hoisted_symbol_identity_keys["existing"] = "original";
	program.funcdef_map["existing"] = &original_func;
	program.namespace_map["known"]["existing"] = &original_var;
	program.struct_map.set("existing", &ddINT32);
	program.deferred_lazy_bodies["existing"].line = 7;
	program.out_of_line_member_defs["existing"].push_back(
		Program::OutOfLineMemberDef());
	program.forest_arena_enabled = true;
	program.fn_template_instantiated.insert("existing");
	program.template_completion_requested.insert("existing");
	program.user_typedef_names.insert("existing");
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
		program.fn_template_instantiated.erase("existing");
		program.fn_template_instantiated.insert("created");
		program.template_completion_requested.erase("existing");
		program.template_completion_requested.insert("created");
		program.user_typedef_names.erase("existing");
		program.user_typedef_names.insert("created");
		project_replacement.type_id = project_original_id;
		CHECK(program.project_types.set(
			project_original_id, &project_replacement));
		uint32_t project_appended_id = program.type_id_for(&project_appended);
		project_appended_replacement.type_id = project_appended_id;
		CHECK(program.project_types.set(
			project_appended_id, &project_appended_replacement));
		program.ast.push_back(&ast_during);
		program.intern_template_param("During", 1);
		Program::PackDeclEntry added_pack_entry = {};
		added_pack_entry.name = "during";
		program.pack_decls.push_back(added_pack_entry);
		program.pack_decl_stack[0].names.push_back(
			std::make_pair("during", Program::pdkStruct));
		program.pack_decl_stack.push_back(Program::PackDeclFrame());
		program.block_typedef_shadows[0].push_back(
			std::make_pair("during", (TokenDataType *)NULL));
		program.block_typedef_shadows.push_back(
			std::vector<std::pair<std::string, TokenDataType *> >());
		Program::ConceptDef replacement_concept;
		replacement_concept.defining_namespace = "replacement";
		program.concept_map["existing"] = replacement_concept;
		program.concept_map["created"] = replacement_concept;
		HoistedDeclIdentity replacement_identity;
		replacement_identity.symbol = "replacement";
		program.function_local_class_identities[&alias_owner] =
			replacement_identity;
		program.hoisted_symbol_identity_keys["existing"] = "replacement";
		program.hoisted_symbol_identity_keys["created"] = "replacement";
		program.set_class_type_alias(&alias_owner, "existing", &ddINT64);
		program.set_class_type_alias(&alias_owner, "existing", &ddUINT64);
		program.set_class_type_alias(&alias_owner, "created", &ddINT64);
		{
			Program::ClassRegistrationJournal nested(program);
			program.set_class_type_alias(&alias_owner, "nested", &ddINT64);
			nested.rollback();
		}
		CHECK(program.active_class_registration_journal != NULL);
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
	CHECK(program.fn_template_instantiated.count("existing") == 1);
	CHECK(program.fn_template_instantiated.count("created") == 0);
	CHECK(program.template_completion_requested.count("existing") == 1);
	CHECK(program.template_completion_requested.count("created") == 0);
	CHECK(program.user_typedef_names.count("existing") == 1);
	CHECK(program.user_typedef_names.count("created") == 0);
	CHECK(program.project_types.size() == 1);
	CHECK(program.project_types.get(project_original_id) == &project_original);
	CHECK(project_original.type_id == project_original_id);
	CHECK(project_replacement.type_id == 0);
	CHECK(project_appended.type_id == 0);
	CHECK(project_appended_replacement.type_id == 0);
	REQUIRE(program.ast.size() == 1);
	CHECK(program.ast[0] == &ast_before);
	REQUIRE(program.template_param_pool.size() == 1);
	CHECK(program.template_param_pool[0]->name == "Before");
	REQUIRE(program.pack_decls.size() == 1);
	CHECK(program.pack_decls[0].name == "before");
	REQUIRE(program.pack_decl_stack.size() == 1);
	REQUIRE(program.pack_decl_stack[0].names.size() == 1);
	CHECK(program.pack_decl_stack[0].names[0].first == "before");
	REQUIRE(program.block_typedef_shadows.size() == 1);
	REQUIRE(program.block_typedef_shadows[0].size() == 1);
	CHECK(program.block_typedef_shadows[0][0].first == "before");
	CHECK(program.concept_map["existing"].defining_namespace == "original");
	CHECK(program.concept_map.count("created") == 0);
	CHECK(program.function_local_class_identities[&alias_owner].symbol ==
	      "original");
	CHECK(program.hoisted_symbol_identity_keys["existing"] == "original");
	CHECK(program.hoisted_symbol_identity_keys.count("created") == 0);
	CHECK(alias_owner.type_aliases["existing"] == &ddINT32);
	CHECK(alias_owner.type_aliases.count("created") == 0);
	CHECK(alias_owner.type_aliases.count("nested") == 0);
	CHECK(program.active_class_registration_journal == NULL);
	CHECK_FALSE(program.class_registration_taps_muted);
	CHECK(program.forest_arena_enabled);
	{
		Program::ClassRegistrationJournal journal(program);
		program.set_class_type_alias(&alias_owner, "committed", &ddINT64);
		program.fn_template_instantiated.insert("committed");
		journal.commit();
	}
	CHECK(alias_owner.type_aliases["committed"] == &ddINT64);
	CHECK(program.fn_template_instantiated.count("committed") == 1);
	CHECK(program.active_class_registration_journal == NULL);
	{
		Program::ClassRegistrationJournal journal(program);
		program.set_class_type_alias(&alias_owner, "committed", &ddUINT64);
		program.fn_template_instantiated.erase("committed");
		journal.rollback();
	}
	CHECK(alias_owner.type_aliases["committed"] == &ddINT64);
	CHECK(program.fn_template_instantiated.count("committed") == 1);
}

TEST_CASE("B3 template lookup stays read-only during registration transactions")
{
	const char *source =
		"template<class T> struct LookupBox { T value; };\n"
		"template<class T> using LookupAlias = LookupBox<T>;\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-readonly-lookup>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));

	madc::dis::intern_keyed_map<std::vector<Program::TemplateDef> >
	    ::transaction_state template_transaction;
	madc::dis::intern_keyed_map<std::vector<Program::TemplateAliasDef> >
	    ::transaction_state alias_transaction;
	program.template_map.begin_transaction(template_transaction);
	program.template_alias_map.begin_transaction(alias_transaction);
	Program::TemplateDef *found_template = program.find_template("LookupBox");
	Program::TemplateDef *found_body = program.template_with_body("LookupBox");
	Program::TemplateDef *missing_template =
		program.find_template("MissingTemplate");
	Program::TemplateAliasDef *found_alias =
		program.find_template_alias("LookupAlias");
	Program::TemplateAliasDef *missing_alias =
		program.find_template_alias("MissingAlias");
	CHECK(found_template != NULL);
	CHECK(found_body != NULL);
	CHECK(missing_template == NULL);
	CHECK(found_alias != NULL);
	CHECK(missing_alias == NULL);
	CHECK(template_transaction.saved.empty());
	CHECK(template_transaction.inserted.empty());
	CHECK(alias_transaction.saved.empty());
	CHECK(alias_transaction.inserted.empty());
	program.template_alias_map.rollback_transaction(alias_transaction);
	program.template_map.rollback_transaction(template_transaction);
}

TEST_CASE("B3 capture indexes nested templates by their parsed owner")
{
	const char *source =
		"template<class T> struct PatternNested {\n"
		"    template<class U> class Rebind { public: U value; };\n"
		"    template<class U> using Pointer = U *;\n"
		"    T value;\n"
		"};\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-nested-templates>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));
	const Program::TemplateDef *definition = program.find_template("PatternNested");
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

TEST_CASE("R1 nested template registrations stay owner-scoped")
{
	const char *source =
		"template<class T> struct R1Outer {\n"
		"    template<class U> struct Rebind { U value; };\n"
		"    template<class U> using Pointer = U *;\n"
		"    T value;\n"
		"};\n"
		"R1Outer<int32_t> first;\n"
		"R1Outer<int64_t> second;\n";
	Program program;
	TokenProgram *tokens = program.tokenize_buffer(
		source, "<class-pattern-owner-registry>");
	REQUIRE(tokens != NULL);
	REQUIRE(program.parse(tokens));

	DataDefCLASS *first = find_canonical_class(
		program, "R1Outer<int32_t>");
	DataDefCLASS *second = find_canonical_class(
		program, "R1Outer<int64_t>");
	REQUIRE(first != NULL);
	REQUIRE(second != NULL);
	Program::TemplateDef *first_rebind =
		program.find_template("Rebind", std::string(), first);
	Program::TemplateDef *second_rebind =
		program.find_template("Rebind", std::string(), second);
	Program::TemplateAliasDef *first_pointer =
		program.find_template_alias("Pointer", std::string(), first);
	Program::TemplateAliasDef *second_pointer =
		program.find_template_alias("Pointer", std::string(), second);
	const std::vector<Program::TemplateDef> *bare_rebind =
		program.template_map.find_readonly("Rebind");
	const std::vector<Program::TemplateAliasDef> *bare_pointer =
		program.template_alias_map.find_readonly("Pointer");
	CHECK(first_rebind != NULL);
	CHECK(second_rebind != NULL);
	CHECK(first_pointer != NULL);
	CHECK(second_pointer != NULL);
	CHECK(bare_rebind == NULL);
	CHECK(bare_pointer == NULL);
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
	const Program::TemplateDef *definition = program.find_template(
		"PatternValueArg");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *pattern =
		program.class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(pattern != NULL);
	CHECK(pattern->capture_reason ==
	      Program::ClassParseReason::DependentValueExpression);
	const Program::TemplateDef *outer_definition = program.find_template(
		"PatternValueOuter");
	REQUIRE(outer_definition != NULL);
	REQUIRE(outer_definition->class_pattern_id != 0);
	const Program::ClassPattern *outer_pattern =
		program.class_pattern_arena.get(outer_definition->class_pattern_id);
	REQUIRE(outer_pattern != NULL);
	// The outer template only passes TYPE arguments; a dependency's own
	// dependent-value ineligibility is not contagious (the resolver
	// instantiates the dependency through the full dispatch, which sends
	// it to the parse lane) — so the outer pattern captures clean.
	CHECK(outer_pattern->capture_reason ==
	      Program::ClassParseReason::None);
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
	const Program::TemplateDef *definition = program.find_template(
		"PatternMaybeUse");
	REQUIRE(definition != NULL);
	REQUIRE(definition->class_pattern_id != 0);
	const Program::ClassPattern *pattern =
		program.class_pattern_arena.get(definition->class_pattern_id);
	REQUIRE(pattern != NULL);
	// No capture-time "member guaranteed" gate: after substitution the
	// source is concrete and the resolver looks the member up through
	// the same machinery the parse lane uses. A binding whose source
	// really lacks the member (the unspecialized PatternMaybe<T>) errors
	// at instantiation — the same diagnostic the parse lane produces —
	// instead of the gate silently poisoning every dependent pattern.
	CHECK(pattern->capture_reason ==
	      Program::ClassParseReason::None);
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

	const Program::TemplateDef *definition = pattern->find_template(
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
