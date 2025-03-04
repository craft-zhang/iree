// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
namespace iree_compiler {
namespace {

// These must match what the runtime uses.
#define IREE_HAL_WEBGPU_PARAMS_BIND_GROUP_INDEX 3
#define IREE_HAL_WEBGPU_PARAMS_BINDING_INDEX 0

static Value convertOpTypeFromI32(IREE::HAL::InterfaceConstantLoadOp loadOp,
                                  tensor::ExtractOp extractOp) {
  OpBuilder builder(loadOp);

  auto loc = loadOp.getLoc();
  auto opType = loadOp.getType();

  // Index
  if (opType.isIndex()) {
    return builder.create<arith::IndexCastOp>(loc, opType, extractOp);
  }

  unsigned sourceBitWidth = 32;
  unsigned destBitWidth = opType.getIntOrFloatBitWidth();

  // AnySignlessInteger
  if (opType.isa<IntegerType>()) {
    if (sourceBitWidth > destBitWidth) {
      return builder.create<arith::TruncIOp>(loc, opType, extractOp);
    } else if (sourceBitWidth < destBitWidth) {
      return builder.create<arith::ExtUIOp>(loc, opType, extractOp);
    } else {
      return extractOp.getResult();
    }
  }

  // AnyFloat
  Value resizedValue = extractOp.getResult();
  if (sourceBitWidth > destBitWidth) {
    return builder.create<arith::TruncFOp>(loc, opType, extractOp);
  } else if (sourceBitWidth < destBitWidth) {
    return builder.create<arith::ExtFOp>(loc, opType, extractOp);
  }
  return builder.create<arith::BitcastOp>(loc, opType, resizedValue);
}

static void replaceConstantLoadOp(IREE::Flow::DispatchTensorLoadOp loadOp,
                                  IREE::HAL::InterfaceConstantLoadOp op) {
  OpBuilder builder(op);

  // tensor.extract -> i32
  auto offsetValue = builder.createOrFold<arith::ConstantIndexOp>(
      op.getLoc(), op.getIndex().getZExtValue());
  auto extractOp =
      builder.create<tensor::ExtractOp>(op.getLoc(), loadOp, offsetValue);

  // i32 -> original type
  auto convertedTypeResult = convertOpTypeFromI32(op, extractOp);
  op.replaceAllUsesWith(convertedTypeResult);

  op.erase();
}

class WGSLReplacePushConstantsPass
    : public WGSLReplacePushConstantsBase<WGSLReplacePushConstantsPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mlir::arith::ArithDialect, mlir::func::FuncDialect,
                    mlir::tensor::TensorDialect, IREE::Flow::FlowDialect,
                    IREE::HAL::HALDialect>();
  }

  void runOnOperation() override {
    auto funcOp = getOperation();
    auto loc = funcOp.getLoc();
    auto constantLoadOps =
        llvm::to_vector<4>(funcOp.getOps<IREE::HAL::InterfaceConstantLoadOp>());
    if (constantLoadOps.empty()) return;

    OpBuilder builder(funcOp);
    builder.setInsertionPointToStart(&funcOp.getBlocks().front());

    // Group all push constants into a single `hal.interface.binding.subspan`
    // and load from it once using `flow.dispatch.tensor.load`, then extract
    // individual push constants with `tensor.extract`.

    // Find the range of push constant indices (0 to some maximum).
    uint64_t maxConstantIndex = 0;
    // Inspect the alignment values. These are just hints, so if all are equal
    // then use the value, otherwise drop the alignment hint.
    SmallVector<uint64_t> alignmentValues;
    bool missingAlignmentValue = false;
    for (auto constantLoadOp : constantLoadOps) {
      maxConstantIndex =
          std::max(constantLoadOp.getIndex().getZExtValue(), maxConstantIndex);

      auto alignmentAttr = constantLoadOp.getAlignmentAttr();
      if (alignmentAttr) {
        uint64_t alignmentValue = alignmentAttr.getValue().getZExtValue();
        alignmentValues.push_back(alignmentValue);
      } else {
        missingAlignmentValue = true;
      }
    }
    auto maxConstantValue =
        builder.create<arith::ConstantIndexOp>(loc, maxConstantIndex);
    mlir::IntegerAttr alignmentAttr = nullptr;
    // TODO(scotttodd): try llvm::all_equal with attrs directly
    if (!missingAlignmentValue && llvm::all_equal(alignmentValues)) {
      alignmentAttr = constantLoadOps[0].getAlignmentAttr();
    }

    // hal.interface.binding.subspan ->
    // !flow.dispatch.tensor<readonly:tensor<Nxi32>>
    //   * Group all push constants into a single tensor<Nxi32>
    //   * If individual data types differ, they'll be bitcast when extracted
    auto dispatchTensorType = IREE::Flow::DispatchTensorType::get(
        IREE::Flow::TensorAccess::ReadOnly,
        {static_cast<int64_t>(maxConstantIndex + 1)}, builder.getI32Type());
    SmallVector<Value> dynamicDims;
    // Note: we're ignoring all potential 'values' hints (if provided) on ops -
    // InterfaceBindingSubspanOp has no matching concept and we assume that any
    // analysis using the hint should have been performed by earlier passes.
    auto subspanOp = builder.create<IREE::HAL::InterfaceBindingSubspanOp>(
        loc, dispatchTensorType,
        /*set=*/APInt(64, IREE_HAL_WEBGPU_PARAMS_BIND_GROUP_INDEX),
        /*binding=*/APInt(64, IREE_HAL_WEBGPU_PARAMS_BINDING_INDEX),
        IREE::HAL::DescriptorType::StorageBuffer, maxConstantValue, dynamicDims,
        alignmentAttr);

    // flow.dispatch.tensor.load -> tensor<Nxi32>
    auto tensorType = RankedTensorType::get({(int64_t)maxConstantIndex + 1},
                                            builder.getI32Type());
    auto loadOp = builder.create<IREE::Flow::DispatchTensorLoadOp>(
        loc, tensorType, subspanOp, dynamicDims);

    // The grouped subspan and load are complete - now extract each constant.
    for (auto constantLoadOp : constantLoadOps) {
      replaceConstantLoadOp(loadOp, constantLoadOp);
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createWGSLReplacePushConstantsPass() {
  return std::make_unique<WGSLReplacePushConstantsPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
