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
 * \file tvm/te/autodiff.h
 * \brief Automatic differentiation of tensor expressions.
 */

#ifndef TVM_TE_AUTODIFF_H
#define TVM_TE_AUTODIFF_H

#include <tvm/runtime/object.h>
#include <tvm/tir/expr.h>
#include "tensor.h"

namespace tvm {
/*! \brief Tensor expression language DSL. */
namespace te {

class DifferentiationResult;

/*! \brief Node to represent a differentiation result */
class DifferentiationResultNode : public Object {
 public:
  /*! \brief The requested adjoints, i.e. Jacobians or gradients wrt to the given inputs */
  Array<Tensor> result;
  /*! \brief A map from tensors to the corresponding adjoints (including internal nodes) */
  Map<Tensor, Tensor> adjoints;
  /*! \brief Single summands of the adjoints */
  Map<Tensor, Map<Tensor, Tensor>> adjoint_summands;
  /*! \brief constructor */
  DifferentiationResultNode() {}

  DifferentiationResultNode(Array<Tensor> result,
                            Map<Tensor, Tensor> adjoints,
                            Map<Tensor, Map<Tensor, Tensor> > adjoint_summands)
    : result(std::move(result)),
      adjoints(std::move(adjoints)),
      adjoint_summands(std::move(adjoint_summands)) {}

  void VisitAttrs(AttrVisitor* v) {
    v->Visit("result", &result);
    v->Visit("adjoints", &adjoints);
    v->Visit("adjoint_summands", &adjoint_summands);
  }

  static constexpr const char* _type_key = "DifferentiationResult";
  TVM_DECLARE_FINAL_OBJECT_INFO(DifferentiationResultNode, Object);
};

class DifferentiationResult : ObjectRef {
 public:
  /*!
   * \brief constructor
   * \param result The requested adjoints.
   * \param adjoints A map from tensors to the corresponding adjoints.
   * \param adjoint_summands A map from tensors to maps from parent tensors
   *        to individual summands of the adjoint.
   */
  TVM_DLL DifferentiationResult(Array<Tensor> result,
                                Map<Tensor, Tensor> adjoints,
                                Map<Tensor, Map<Tensor, Tensor> > adjoint_summands);

  // declare DifferentiationResult.
  TVM_DEFINE_OBJECT_REF_METHODS(DifferentiationResult, ObjectRef, DifferentiationResultNode);
};

/*!
 * \brief Take the derivative of the expression with respect to the given variable.
 * \param expr The expression to differentiate.
 * \param var The variable to differentiate with respect to.
 * \return The expression for the derivative.
 */
TVM_DLL PrimExpr Derivative(const PrimExpr& expr, const Var& var);

/*!
 * \brief Get the tensor representing the Jacobian of the output with respect to the input.
 *
 *  Note that if \p output depends on \p input indirectly (by using some other tensor
 *  depending on \p input), this dependency won't contribute to the resulting Jacobian.
 *  For such cases use the function ::Differentiate.
 *
 * \param output The tensor to differentiate.
 * \param input The input tensor, which \p output should directly use.
 * \param optimize Whether to perform optimizations like lifting of nonzeroness conditions.
 * \return The tensor representing the Jacobian of shape `output.shape + input.shape`.
 */
TVM_DLL Tensor Jacobian(const Tensor& output, const Tensor& input, bool optimize = true);

/*! \brief A type of a "local" differentiation function for reverse mode AD
 *
 *  A function of this type is a building block for reverse-mode automatic differentiation. It
 *  should take three tensors: `output`, `input` and `head`, `head` being the adjoint corresponding
 *  to the `output`, and return (a summand of) the adjoint corresponding to the input. In other
 *  words, it should differentiate `output` wrt `input` and multiply the result by `head` with
 *  tensor dot product (`head` should be on the left of the multiplication). `input` should be an
 *  immediate dependency of `output` (should be called from within the body of `output`).
 *
 *  See also ::DiffBuildingBlock, which might be considered the reference implementation.
 */
using FDiffBuildingBlock = std::function<Tensor(const Tensor& output,
                                                const Tensor& input,
                                                const Tensor& head)>;

/*!
 * \brief The building block for reverse-mode AD.
 *
 *  Differentiate \p output wrt \p input and multiply the result by \p head on the left using tensor
 *  dot product. \p input must be an immediate dependency of \p output (must be called from within
 *  the body of \p output). That is, the function will compute a summand of the adjoint for \p input
 *  given the adjoint for \p output (which is called \p head here).
 *
 * \param output The tensor to differentiate.
 * \param input The input tensor, which \p output should directly use.
 * \param head The adjoint of \p output. Must be of shape `prefix + output.shape`
 * \return The tensor representing the adjoint of \p input of shape `prefix + input.shape`.
 */
TVM_DLL Tensor DiffBuildingBlock(const Tensor& output, const Tensor& input, const Tensor& head);

/*!
 * \brief Perform reverse mode automatic differentiation.
 *
 *  Each item of the `result` field of the result is an adjoint for the corresponding item of
 *  \p inputs, i.e. \p head multiplied by the Jacobian of \p output with respect to the
 *  corresponding item of \p inputs.
 *
 * \param output The tensor to differentiate.
 * \param inputs The array of input tensors. When the array is empty, will perform differentiation
 *               wrt all tensors the output depends on.
 * \param head The adjoint of the output, in other words, some tensor, by which the Jacobians
 *             will be multiplied. Its shape must be of the form `prefix + output.shape`. If the
 *             null pointer is provided, the identity tensor of shape
 *             `output.shape + output.shape` will be used.
 * \param fdiff The function performing differentiation and multiplication, see
 *              ::FDiffBuildingBlock.
 * \param override_deps A map from tensors to their dependencies (`InputTensors()` are used by
 *                      default). Overriding dependencies may be useful to treat a group of tensors
 *                      as a single supertensor. In this case the fdiff functions should also be
 *                      modified accordingly.
 * \return An object of type DifferentiationResult which contains three fields:
 *         - `result` An array of adjoints corresponding to \p inputs.
 *         - `adjoints` A map from tensors to the corresponding adjoints (includes intermediate
 *            tensors).
 *         - `adjoint_summands` A map from tensors to maps from parent tensors to individual
 *            summands of the adjoint.
 */
TVM_DLL DifferentiationResult Differentiate(
    const Tensor& output,
    const Array<Tensor>& inputs = Array<Tensor>(),
    const Tensor& head = Tensor(),
    const FDiffBuildingBlock& fdiff = DiffBuildingBlock,
    const Map<Tensor, Array<Tensor>>& override_deps = Map<Tensor, Array<Tensor>>());

}  // namespace te
}  // namespace tvm

#endif //TVM_TE_AUTODIFF_H
