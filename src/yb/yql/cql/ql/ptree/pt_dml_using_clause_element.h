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

#ifndef YB_YQL_CQL_QL_PTREE_PT_DML_USING_CLAUSE_ELEMENT_H
#define YB_YQL_CQL_QL_PTREE_PT_DML_USING_CLAUSE_ELEMENT_H

#include "yb/yql/cql/ql/ptree/tree_node.h"
#include "yb/yql/cql/ql/ptree/pt_expr.h"

namespace yb {
namespace ql {

// USING clause for INSERT, UPDATE and DELETE statements.
class PTDmlUsingClauseElement : public TreeNode {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef MCSharedPtr<PTDmlUsingClauseElement> SharedPtr;
  typedef MCSharedPtr<const PTDmlUsingClauseElement> SharedPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructors and destructor.
  PTDmlUsingClauseElement(MemoryContext *memctx,
                          YBLocation::SharedPtr loc,
                          const MCSharedPtr<MCString>& name,
                          const PTExpr::SharedPtr& value);

  virtual ~PTDmlUsingClauseElement();

  template<typename... TypeArgs>
  inline static PTDmlUsingClauseElement::SharedPtr MakeShared(MemoryContext *memctx,
                                                       TypeArgs&&... args) {
    return MCMakeShared<PTDmlUsingClauseElement>(memctx, std::forward<TypeArgs>(args)...);
  }

  // Node semantics analysis.
  virtual CHECKED_STATUS Analyze(SemContext *sem_context) override;

  const PTExpr::SharedPtr value() {
    return value_;
  }

  bool IsTTL() const {
    return strcmp(name_->c_str(), kTtl) == 0;
  }

  bool IsTimestamp() const {
    return strcmp(name_->c_str(), kTimestamp) == 0;
  }

 private:
  constexpr static const char* const kTtl = "ttl";
  constexpr static const char* const kTimestamp = "timestamp";

  const MCSharedPtr<MCString> name_;
  const PTExpr::SharedPtr value_;
};

} // namespace ql
} // namespace yb

#endif // YB_YQL_CQL_QL_PTREE_PT_DML_USING_CLAUSE_ELEMENT_H
