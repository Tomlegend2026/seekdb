/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/engine/basic/file_scan_native.h"
#include "sql/parser/ob_parser.h"
#include "sql/optimizer/ob_optimizer_util.h"
#include "sql/privilege_check/ob_privilege_check.h"
#include "sql/ob_sql_context.h"
#include <map>

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

namespace
{
// Recursive CTEs reference their own definition, so every walk is depth limited.
const int MAX_FILE_DESCRIPTION_DEPTH = 16;

bool is_file_table(const TableItem *table)
{
  return table != nullptr && table->json_table_def_ != nullptr
      && table->json_table_def_->table_type_ == OB_FILE_TABLE_TYPE;
}

// Derived tables, CTE references and CTE views keep their subquery in
// ref_query_. A CTE used in the FROM clause is materialised as a temp table.
bool is_indirect_table(const TableItem *table)
{
  return table != nullptr && table->ref_query_ != nullptr
      && (table->is_generated_table() || table->is_fake_cte_table() || table->is_temp_table());
}

int contains_file_table(const ObSelectStmt &query, bool &found, int depth)
{
  int ret = OB_SUCCESS;
  if (depth >= MAX_FILE_DESCRIPTION_DEPTH) { return ret; }
  for (int64_t i = 0; i < query.get_table_items().count(); ++i) {
    const TableItem *table = query.get_table_items().at(i);
    if (is_file_table(table)) { found = true; }
    else if (is_indirect_table(table)) {
      OZ (contains_file_table(*table->ref_query_, found, depth + 1));
    }
    if (found) { break; }
  }
  const auto &cte_definitions = query.get_cte_definitions();
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < cte_definitions.count(); ++i) {
    const TableItem *cte = cte_definitions.at(i);
    if (OB_ISNULL(cte)) { ret = OB_ERR_UNEXPECTED; }
    else if (cte->ref_query_ != nullptr) {
      OZ (contains_file_table(*cte->ref_query_, found, depth + 1));
    }
  }
  if (OB_SUCC(ret) && !found) {
    ObSEArray<ObSelectStmt *, 4> children;
    OZ (query.get_child_stmts(children));
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < children.count(); ++i) {
      if (OB_ISNULL(children.at(i))) { ret = OB_ERR_UNEXPECTED; }
      else { OZ (SMART_CALL(contains_file_table(*children.at(i), found, depth + 1))); }
    }
  }
  return ret;
}

// A query that also reads a protected table must not be answered with file
// metadata: the executor checks the original query privileges for the normal
// EXPLAIN output, and that check is what keeps the protected schema private.
int contains_protected_table(const ObSelectStmt &query, bool &found, int depth)
{
  int ret = OB_SUCCESS;
  if (depth >= MAX_FILE_DESCRIPTION_DEPTH) { return ret; }
  for (int64_t i = 0; i < query.get_table_items().count(); ++i) {
    const TableItem *table = query.get_table_items().at(i);
    if (OB_ISNULL(table)) { ret = OB_ERR_UNEXPECTED; break; }
    if (table->is_basic_table() || table->is_view_table_) { found = true; }
    else if (is_indirect_table(table)) {
      OZ (contains_protected_table(*table->ref_query_, found, depth + 1));
    }
    if (found) { break; }
  }
  const auto &cte_definitions = query.get_cte_definitions();
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < cte_definitions.count(); ++i) {
    const TableItem *cte = cte_definitions.at(i);
    if (OB_ISNULL(cte)) { ret = OB_ERR_UNEXPECTED; }
    else if (cte->ref_query_ != nullptr) {
      OZ (contains_protected_table(*cte->ref_query_, found, depth + 1));
    }
  }
  if (OB_SUCC(ret) && !found) {
    ObSEArray<ObSelectStmt *, 4> children;
    OZ (query.get_child_stmts(children));
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < children.count(); ++i) {
      if (OB_ISNULL(children.at(i))) { ret = OB_ERR_UNEXPECTED; }
      else { OZ (SMART_CALL(contains_protected_table(*children.at(i), found, depth + 1))); }
    }
  }
  return ret;
}

// Hex literals preserve arbitrary UTF-8 names, quotes and SQL-mode semantics.
std::string metadata_literal(const ObString &text)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string result = "CONVERT(X'";
  for (int64_t i = 0; i < text.length(); ++i) {
    const auto byte = static_cast<unsigned char>(text.ptr()[i]);
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result + "' USING utf8mb4)";
}

const char *file_type_name(file_scan::Type type)
{
  using file_scan::Type;
  switch (type) {
    case Type::BIGINT: return "BIGINT";
    case Type::DOUBLE: return "DOUBLE";
    case Type::BOOLEAN: return "BOOLEAN";
    case Type::DATE: return "DATE";
    case Type::DATETIME: return "DATETIME";
    default: return "VARCHAR";
  }
}

struct FileColumnMeta
{
  std::string type_name;
  bool nullable = true;
};

bool same_column_name(const ObString &left, const ObString &right)
{
  if (left.length() != right.length()) { return false; }
  for (int64_t i = 0; i < left.length(); ++i) {
    char l = left.ptr()[i];
    char r = right.ptr()[i];
    if (l >= 'A' && l <= 'Z') { l = static_cast<char>(l - 'A' + 'a'); }
    if (r >= 'A' && r <= 'Z') { r = static_cast<char>(r - 'A' + 'a'); }
    if (l != r) { return false; }
  }
  return true;
}

// Locate the select item of a derived table or CTE that produces the referenced
// column. Derived tables and CTE views number their columns from
// OB_APP_MIN_COLUMN_ID, while a CTE over a base table keeps the base table
// numbering, so both bases are probed and the column name breaks the tie.
int find_subquery_item(const ObSelectStmt &sub_query,
                       const ObColumnRefRawExpr &column,
                       int64_t &index)
{
  index = -1;
  const int64_t candidates[2] = {
      static_cast<int64_t>(column.get_column_id()) - static_cast<int64_t>(common::OB_APP_MIN_COLUMN_ID),
      static_cast<int64_t>(column.get_column_id()) - 1};
  const ObString &name = column.get_column_name();
  for (int64_t c = 0; c < 2; ++c) {
    const int64_t i = candidates[c];
    if (i < 0 || i >= sub_query.get_select_item_size()) { continue; }
    if (index < 0) { index = i; }
    const auto &item = sub_query.get_select_item(i);
    const ObString &item_name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
    if (!name.empty() && !item_name.empty() && same_column_name(name, item_name)) {
      index = i;
      break;
    }
  }
  return OB_SUCCESS;
}

// Resolve one output column. Column references are followed into derived tables
// and CTE definitions so the file specific type name and the inferred
// nullability survive the projection instead of degrading to the generic type.
int describe_column(const ObSelectStmt &query,
                    const SelectItem &item,
                    const file_scan::FilePolicy &policy,
                    std::map<uint64_t, file_scan::CsvSchema> &schemas,
                    FileColumnMeta &meta,
                    int depth)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(item.expr_)) { return OB_ERR_UNEXPECTED; }
  const auto &type = item.expr_->get_result_type();
  meta.type_name = ob_sql_type_str(type.get_type());
  meta.nullable = !type.is_not_null_for_read();
  if (!item.expr_->is_column_ref_expr()) { return ret; }

  const auto &column = static_cast<const ObColumnRefRawExpr &>(*item.expr_);
  const auto *table = query.get_table_item_by_id(column.get_table_id());
  if (OB_ISNULL(table)) { return ret; }

  bool null_side = false;
  OZ (ObOptimizerUtil::is_table_on_null_side(&query, table->table_id_, null_side));
  if (null_side) { meta.nullable = true; }

  if (is_file_table(table)) {
    auto schema = schemas.find(table->table_id_);
    if (schema == schemas.end()) {
      file_scan::QueryOptions options;
      file_scan::FileCursor cursor;
      const auto &metadata = table->json_table_def_->namespace_arr_;
      OZ (file_scan::query_from_metadata(metadata, options));
      if (OB_SUCC(ret)) {
        const auto status = cursor.prepare(policy, options);
        OZ (file_scan::diagnostic_error(status, cursor.diagnostic()));
      }
      if (OB_SUCC(ret)) {
        const ObString bound_fingerprint = metadata.at(7);
        const std::string fingerprint(bound_fingerprint.ptr(), bound_fingerprint.length());
        if (cursor.fingerprint() != fingerprint) { ret = OB_SCHEMA_EAGAIN; }
        else { schema = schemas.emplace(table->table_id_, cursor.schema()).first; }
      }
    }
    const uint64_t index = column.get_column_id() - 1;
    if (OB_SUCC(ret)) {
      if (index >= schema->second.types.size()
          || index >= schema->second.nullable.size()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        meta.type_name = file_type_name(schema->second.types[index]);
        meta.nullable = schema->second.nullable[index] || null_side;
      }
    }
  } else if (depth < MAX_FILE_DESCRIPTION_DEPTH && is_indirect_table(table)) {
    const ObSelectStmt &sub_query = *table->ref_query_;
    int64_t index = -1;
    OZ (find_subquery_item(sub_query, column, index));
    if (index >= 0 && index < sub_query.get_select_item_size()) {
      FileColumnMeta sub_meta;
      OZ (describe_column(sub_query, sub_query.get_select_item(index),
                           policy, schemas, sub_meta, depth + 1));
      meta.type_name = sub_meta.type_name;
      meta.nullable = sub_meta.nullable || null_side;
    }
  }
  return ret;
}
} // namespace

int ObExplainResolver::resolve_file_description(ObSelectStmt &query, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  bool has_file = false;
  OZ (contains_file_table(query, has_file, 0));
  if (OB_SUCC(ret) && has_file) {
    // The metadata only describes file columns. If the query also reads a
    // protected table, keep the normal EXPLAIN output so the executor still
    // checks the privileges of the original query.
    bool has_protected = false;
    OZ (contains_protected_table(query, has_protected, 0));
    if (OB_SUCC(ret) && has_protected) { return ret; }
    handled = true;
    // The metadata SELECT replaces the original statement. The file access
    // itself is guarded by the FILE privilege and secure_file_priv.
    file_scan::FilePolicy policy;
    OZ (file_scan::session_policy(*session_info_, policy));
    std::map<uint64_t, file_scan::CsvSchema> schemas;
    std::string sql;
    for (int64_t i = 0; OB_SUCC(ret) && i < query.get_select_item_size(); ++i) {
      const auto &item = query.get_select_item(i);
      FileColumnMeta meta;
      OZ (describe_column(query, item, policy, schemas, meta, 0));
      if (OB_SUCC(ret)) {
        if (i > 0) { sql += " UNION ALL "; }
        sql += "SELECT " + metadata_literal(item.alias_name_.empty() ? item.expr_name_ : item.alias_name_)
            + " AS column_name," + metadata_literal(ObString::make_string(meta.type_name.c_str()))
            + " AS column_type,'" + (meta.nullable ? "YES" : "NO")
            + "' AS `null`,NULL AS `key`,NULL AS `default`,NULL AS extra";
      }
    }
    if (OB_SUCC(ret)) {
      ObParser parser(*allocator_, session_info_->get_sql_mode());
      ParseResult result;
      OZ (parser.parse(ObString(sql.size(), sql.data()), result));
      if (OB_SUCC(ret)) {
        if (OB_ISNULL(result.result_tree_) || result.result_tree_->num_child_ != 1
            || OB_ISNULL(result.result_tree_->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ObResolver resolver(params_);
          ObStmt *description = nullptr;
          OZ (resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                              *result.result_tree_->children_[0], description));
          if (OB_SUCC(ret)) { set_basic_stmt(description); }
        }
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_BOOL: // Plain DESCRIBE/DESC; FileSQL exposes result metadata below.
            break;
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  if (OB_SUCC(ret) && parse_tree.children_[EXPLAIN_FORMAT] != nullptr
      && parse_tree.children_[EXPLAIN_FORMAT]->type_ == T_BOOL && explain_stmt != nullptr
      && explain_stmt->get_explain_query_stmt()->is_select_stmt()) {
    bool handled = false;
    OZ (resolve_file_description(
        *static_cast<ObSelectStmt *>(explain_stmt->get_explain_query_stmt()), handled));
  }
  return ret;
}
