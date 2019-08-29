/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  Copyright (c) 2018 by Contributors
 * \file layout_infer.cc
 * \brief Relay layout inference and checking.
 *
 * This file implements one of the most important passes to the
 * Relay IR. In order to do many transformations and generate the
 * most efficient code we need to obtain type information for the
 * IR.
 *
 * Like computation graphs the IR leaves most type information
 * implicit and relies performing analysis of the program to
 * generate this information.
 *
 * This pass given an expression `e` will infer a type `t` for
 * the expression simultaneous checking the property `e : t`
 * (i.e we can show e has type t).
 *
 * If we can not infer a type or there are conflicting typing
 * constraints we will trigger an error.
 */

#include <tvm/relay/error.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/relay/pattern_functor.h>
#include <tvm/data_layout.h>
#include <tvm/relay/transform.h>
#include <tvm/relay/layout.h>
#include "./pass_util.h"
#include "../ir/type_functor.h"

namespace tvm {
namespace relay {

class LayoutInferencer : private ExprFunctor<RelayLayout(const Expr&)> {
 public:
  LayoutInferencer() : modified_(false), timestamp_(0) {}

  explicit LayoutInferencer(Map<Expr, RelayLayout>& in_layouts) : layout_map_(in_layouts) {}

  // inference the type of expr.
  Expr Infer(Expr expr) {
    this->VisitExpr(expr);
    while (modified_) {
      modified_ = false;
      timestamp_++;
      this->Infer(expr);
    }
    return expr;
  }

  Map<Expr, Array<Layout> > CollectLayoutInfo() {
    Map<Expr, Array<Layout> > map;
    for (auto& iter : layout_map_) {
      auto layout = iter.second;
      if (auto* tensor_layout = layout.as<TensorLayoutNode>()) {
        map.Set(iter.first, Array<Layout>({tensor_layout->layout}));
      } else {
        auto* tuple_layout = layout.as<TupleLayoutNode>();
        CHECK(tuple_layout);
        map.Set(iter.first, tuple_layout->fields);
      }
    }
    return map;
  }

 private:
  // map from expression to checked type
  // type inferencer will populate it up
  // std::unordered_map<Expr, RelayLayout, NodeHash, NodeEqual> layout_map_;
  Map<Expr, RelayLayout> layout_map_;
  std::unordered_map<Expr, int, NodeHash, NodeEqual> layout_timestamp_;
  bool modified_;
  int timestamp_;

  RelayLayout GetLayout(const Expr& expr) {
    auto it = layout_map_.find(expr);
    if (it == layout_map_.end() || layout_timestamp_[expr] < timestamp_) {
      auto layout = this->VisitExpr(expr);
      layout_map_.Set(expr, layout);
    }
    return layout_map_[expr];
  }

  RelayLayout GetCachedLayout(const Expr& expr, const Layout& default_layout = Layout::Undef()) {
    if (layout_map_.count(expr)) {
      return layout_map_[expr];
    }
    const size_t num_outputs = expr->checked_type()->is_type<TupleTypeNode>() ?
                               expr->type_as<TupleTypeNode>()->fields.size() : 1;
    RelayLayout olayout;
    if (num_outputs == 1) {
      olayout = TensorLayoutNode::make(default_layout);
    } else {
      olayout = TupleLayoutNode::make(Array<Layout>(num_outputs, default_layout));
    }
    modified_ = true;
    layout_map_.Set(expr, olayout);
    return olayout;
  }

  void UpdateLayoutCache(const Expr& expr, const RelayLayout& layout) {
    if (!layout_map_.count(expr) || !layout_map_[expr].Equals(layout)) {
      layout_map_.Set(expr, layout);
      modified_ = true;
    }
  }

  void UpdateLayoutCache(const LayoutReporter& reporter) {
    for (auto& it : reporter->results) {
      UpdateLayoutCache(it.first, it.second);
    }
  }

  // Visitor Logic
  RelayLayout VisitExpr_(const VarNode* op) final {
    return GetCachedLayout(GetRef<Var>(op));
  }

  RelayLayout VisitExpr_(const GlobalVarNode* op) final {
    // module.lookup(var)
    LOG(FATAL) << "GlobalVarNode";
  }

  RelayLayout VisitExpr_(const ConstantNode* op) final {
    LOG(FATAL) << "ConstantNode";
  }

  RelayLayout VisitExpr_(const TupleNode* op) final {
    LOG(FATAL) << "TupleNode";
  }

  RelayLayout VisitExpr_(const TupleGetItemNode* op) final {
    LOG(FATAL) << "TupleGetItemNode";
  }

  RelayLayout VisitExpr_(const OpNode* op) final {
    LOG(FATAL) << "OpNode";
  }

  RelayLayout VisitExpr_(const LetNode* let) final {
    LOG(FATAL) << "LetNode";
  }

  RelayLayout VisitExpr_(const IfNode* ite) final {
    LOG(FATAL) << "IfNode";
  }

  RelayLayout VisitExpr_(const CallNode* call) final {
    auto node = GetRef<Call>(call);
    Array<RelayLayout> layouts;
    Array<Type> types;
    Array<Expr> nodes;

    for (auto arg : call->args) {
      auto arg_layout = GetLayout(arg);
      layouts.push_back(arg_layout);
      types.push_back(arg->checked_type());
      nodes.push_back(arg);
    }

    layouts.push_back(GetCachedLayout(node));
    types.push_back(call->checked_type());
    nodes.push_back(GetRef<Call>(call));

    static auto finfer_layout = Op::GetAttr<FInferLayout>("FInferLayout");

    Op op = Downcast<Op>(call->op);
    if (finfer_layout.count(op)) {
      auto reporter = LayoutReporterNode::make(nodes, layouts);
      bool infer_success = finfer_layout[op](layouts, types, call->args.size(), call->attrs, reporter);
      if (infer_success) {
        UpdateLayoutCache(reporter);
      }
    }
    layout_timestamp_[node] = timestamp_;
    return GetCachedLayout(node);
  }

  RelayLayout VisitExpr_(const FunctionNode* f) final {
    // TODO: params
    return this->VisitExpr(f->body);
  }

  RelayLayout VisitExpr_(const MatchNode* op) final {
    LOG(FATAL) << "MatchNode";
  }

  RelayLayout VisitExpr_(const RefCreateNode* op) final {
    LOG(FATAL) << "RefCreateNode";
  }

  RelayLayout VisitExpr_(const RefReadNode* op) final {
    LOG(FATAL) << "RefReadNode";
  }

  RelayLayout VisitExpr_(const RefWriteNode* op) final {
    LOG(FATAL) << "RefWriteNode";
  }

  RelayLayout VisitExpr_(const ConstructorNode* c) final {
    LOG(FATAL) << "ConstructorNode";
  }
};

//Expr InferLayout(const Expr& expr, const Module& mod_ref) {
//  // TODO
//  return LayoutInferencer().Infer(expr);
//}

Map<Expr, Array<Layout> > CollectLayoutInfo(const Expr& expr, Map<Expr, RelayLayout> in_layouts) {
  LayoutInferencer inferencer(in_layouts);
  inferencer.Infer(expr);
  return inferencer.CollectLayoutInfo();
}

//TVM_REGISTER_API("relay._analysis.CollectLayoutInfo")
//.set_body_typed(CollectLayoutInfo);

TVM_REGISTER_API("relay._analysis.CollectLayoutInfo")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = CollectLayoutInfo(args[0].operator Expr(), args[1].operator Map<Expr, RelayLayout>());
  });

//namespace transform {

//Pass InferLayout() {
//  runtime::TypedPackedFunc<Function(Function, Module, PassContext)> pass_func =
//      [=](Function f, Module m, PassContext pc) {
//        return Downcast<Function>(InferLayout(f, m));
//      };
//  return CreateFunctionPass(pass_func, 0, "InferLayout", {ir::StringImm::make("InferType")});
//}

//TVM_REGISTER_API("relay._transform.InferLayout")
//.set_body_typed<Pass()>([]() {
//  return InferLayout();
//});

//}  // namespace transform

}  // namespace relay
}  // namespace tvm
