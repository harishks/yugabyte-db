// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/yql/cql/ql/ptree/pt_property.h"
#include "yb/util/stol_utils.h"
#include "yb/util/string_case.h"

namespace yb {
namespace ql {

using strings::Substitute;

PTProperty::PTProperty(MemoryContext *memctx,
                      YBLocation::SharedPtr loc,
                      const MCSharedPtr<MCString>& lhs,
                      const PTExpr::SharedPtr& rhs)
    : TreeNode(memctx, loc),
      lhs_(lhs),
      rhs_(rhs) {
}

PTProperty::PTProperty(MemoryContext *memctx,
                       YBLocation::SharedPtr loc)
    : TreeNode(memctx, loc) {
}

PTProperty::~PTProperty() {
}

Status PTProperty::GetIntValueFromExpr(PTExpr::SharedPtr expr,
                                       const string& property_name,
                                       int64_t *val) {
  DCHECK_ONLY_NOTNULL(val);

  if (expr == nullptr) {
    return STATUS(InvalidArgument, Substitute("Invalid integer value for '$0'", property_name));
  }

  if (expr->ql_type_id() == DataType::VARINT || expr->ql_type_id() == DataType::STRING) {
    MCSharedPtr<MCString> str_val;
    if (expr->ql_type_id() == DataType::STRING) {
      str_val = std::dynamic_pointer_cast<PTConstText>(expr)->Eval();
    } else {
      DCHECK(expr->ql_type_id() == DataType::VARINT);
      str_val = std::dynamic_pointer_cast<PTConstVarInt>(expr)->Eval();
    }
    return ResultToStatus(&util::CheckedStoll)(val, *str_val);
  } else if (QLType::IsInteger(expr->ql_type_id())) {
    *val = std::dynamic_pointer_cast<PTConstInt>(expr)->Eval();
    return Status::OK();
  }
  return STATUS(InvalidArgument, Substitute("Invalid integer value for '$0'", property_name));
}

Status PTProperty::GetDoubleValueFromExpr(PTExpr::SharedPtr expr,
                                          const string& property_name,
                                          long double *val) {
  DCHECK_ONLY_NOTNULL(val);

  if (expr == nullptr) {
    return STATUS_FORMAT(InvalidArgument, "Invalid float value for '$0'", property_name);
  }
  if (QLType::IsNumeric(expr->ql_type_id())) {
    if (QLType::IsInteger(expr->ql_type_id())) {
      DCHECK(expr->ql_type_id() == DataType::VARINT);
      RETURN_NOT_OK(std::static_pointer_cast<PTConstVarInt>(expr)->ToDouble(val, false));
    } else {
      DCHECK(expr->ql_type_id() == DataType::DECIMAL);
      RETURN_NOT_OK(std::static_pointer_cast<PTConstDecimal>(expr)->ToDouble(val, false));
    }
    return Status::OK();
  } else if (expr->ql_type_id() == DataType::STRING) {
    auto str_val = std::dynamic_pointer_cast<PTConstText>(expr)->Eval();
    return ResultToStatus(&util::CheckedStold)(val, *str_val);
  }
  return STATUS_FORMAT(InvalidArgument, "Invalid float value for '$0'", property_name);
}

Status PTProperty::GetBoolValueFromExpr(PTExpr::SharedPtr expr,
                                        const string& property_name,
                                        bool *val) {
  DCHECK_ONLY_NOTNULL(val);

  if (expr == nullptr) {
    return STATUS(InvalidArgument, Substitute("'$0' should either be true or false",
                                              property_name));
  }
  if (expr->ql_type_id() == DataType::BOOL) {
    *val = std::dynamic_pointer_cast<PTConstBool>(expr)->Eval();
    return Status::OK();
  } else if (expr->ql_type_id() == DataType::STRING) {
    auto mcstr = std::dynamic_pointer_cast<PTConstText>(expr)->Eval();
    string str_val;
    ToLowerCase(mcstr->c_str(), &str_val);
    if (str_val == "true") {
      *val = true;
      return Status::OK();
    } else if (str_val == "false") {
      *val = false;
      return Status::OK();
    }
    return STATUS(InvalidArgument, Substitute("'$0' should either be true or false, not $1",
                                              property_name, str_val));
  }
  return STATUS(InvalidArgument, Substitute("'$0' should either be true or false", property_name));
}

Status PTProperty::GetStringValueFromExpr(PTExpr::SharedPtr expr,
                                          bool to_lower_case,
                                          const string& property_name,
                                          string *val) {
  DCHECK_ONLY_NOTNULL(val);

  if (expr && expr->ql_type_id() == DataType::STRING) {
    auto mcstr = std::dynamic_pointer_cast<PTConstText>(expr)->Eval();
    if (to_lower_case) {
      ToLowerCase(mcstr->c_str(), val);
    } else {
      *val = mcstr->c_str();
    }
    return Status::OK();
  }
  return STATUS(InvalidArgument, Substitute("Invalid string value for '$0'", property_name));
}

bool PTProperty::StringEndsWith(const string& s, const char *end, size_t end_len, string *left) {
  // For our purpose, s should always have at least one character before the string we are looking
  // for.
  if (s.length() <= end_len) {
    return false;
  }
  if (s.find(end, s.length() - end_len) != string::npos) {
    if (left != nullptr) {
      *left = s.substr(0, s.length() - end_len);
    }
    return true;
  }
  return false;
}

} // namespace ql
} // namespace yb
