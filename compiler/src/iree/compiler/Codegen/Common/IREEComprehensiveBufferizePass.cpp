// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- IREEComprehensiveBufferizePass.cpp - -------------------------===//
//
// Wrapper pass to use MLIRs ComprehensiveBufferization pass.
//
//===----------------------------------------------------------------------===//
#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree/compiler/Codegen/Common/BufferizationAnalysis.h"
#include "iree/compiler/Codegen/Interfaces/BufferizationInterfaces.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/Flow/IR/FlowTypes.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "iree/compiler/Dialect/Util/IR/UtilDialect.h"
#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/ErrorHandling.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/BufferUtils.h"
#include "mlir/Dialect/Bufferization/Transforms/EmptyTensorElimination.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

#define DEBUG_TYPE "iree-codegen-linalg-bufferize"

using mlir::bufferization::BufferizationOptions;
using mlir::bufferization::OneShotAnalysisState;
using mlir::bufferization::OneShotBufferizationOptions;

namespace mlir {
namespace iree_compiler {

namespace {
class EliminateEmptyTensorsPass
    : public EliminateEmptyTensorsBase<EliminateEmptyTensorsPass> {
 public:
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<IREE::Flow::FlowDialect, tensor::TensorDialect>();
  }

  void runOnOperation() override;
};

/// Pass to convert from tensor based ops to memref based ops.
class IREEComprehensiveBufferizePass
    : public IREEComprehensiveBufferizeBase<IREEComprehensiveBufferizePass> {
 public:
  explicit IREEComprehensiveBufferizePass(
      Optional<BufferizationOptions::AllocationFn> allocationFn = None,
      Optional<BufferizationOptions::DeallocationFn> deallocationFn = None,
      Optional<BufferizationOptions::MemCpyFn> memCpyFn = None)
      : allocationFn(allocationFn),
        deallocationFn(deallocationFn),
        memCpyFn(memCpyFn) {}

  void getDependentDialects(DialectRegistry &registry) const override {
    // clang-format off
    registry
        .insert<AffineDialect,
                arith::ArithDialect,
                bufferization::BufferizationDialect,
                func::FuncDialect,
                IREE::Flow::FlowDialect,
                IREE::Util::UtilDialect,
                linalg::LinalgDialect,
                memref::MemRefDialect,
                scf::SCFDialect,
                tensor::TensorDialect,
                vector::VectorDialect>();
    // clang-format on
  }

  void runOnOperation() override;

 private:
  const Optional<BufferizationOptions::AllocationFn> allocationFn;
  const Optional<BufferizationOptions::DeallocationFn> deallocationFn;
  const Optional<BufferizationOptions::MemCpyFn> memCpyFn;
};
}  // namespace

static bool isaTensor(Type t) { return t.isa<TensorType>(); };

// Default allocation functions.
static FailureOr<Value> defaultAllocationFn(OpBuilder &builder, Location loc,
                                            MemRefType allocationType,
                                            ValueRange dynamicSizes,
                                            unsigned int alignment) {
  return builder.create<memref::AllocOp>(loc, allocationType, dynamicSizes)
      .getResult();
}
static LogicalResult defaultDeallocationFn(OpBuilder &builder, Location loc,
                                           Value allocation) {
  builder.create<memref::DeallocOp>(loc, allocation);
  return success();
}
static LogicalResult defaultMemCpyFn(OpBuilder &builder, Location loc,
                                     Value from, Value to) {
  Operation *copyOp = createLinalgCopyOp(builder, loc, from, to);
  return success(static_cast<bool>(copyOp));
}

OneShotBufferizationOptions getBufferizationOptions() {
  OneShotBufferizationOptions options;

  // bufferization.to_memref is used to bufferize constants in IREE. IREE has
  // it's own logic to handle constants. We'd like to leave the arith.constant
  // as is and insert bufferization.to_memref to convert the tensor to memref.
  options.opFilter.denyOperation<arith::ConstantOp>();
  options.opFilter.denyOperation<bufferization::ToMemrefOp>();

  // This type converter converts tensor types to memref types when no exact
  // memref type can be inferred from the context.
  options.unknownTypeConverterFn = [](Value value, unsigned memorySpace,
                                      const BufferizationOptions &options) {
    auto tensorType = value.getType().cast<TensorType>();

    // Special rule for ConstantOps: These always lower to some memref with a
    // static identity layout.
    if (value.getDefiningOp<arith::ConstantOp>())
      return bufferization::getMemRefTypeWithStaticIdentityLayout(tensorType,
                                                                  memorySpace);

    // Default case: Fully dynamic layout map for best compatibility.
    return bufferization::getMemRefTypeWithFullyDynamicLayout(tensorType,
                                                              memorySpace);
  };

  return options;
}

void EliminateEmptyTensorsPass::runOnOperation() {
  ModuleOp moduleOp = getOperation();

  // Analyze IR.
  OneShotBufferizationOptions options = getBufferizationOptions();
  OneShotAnalysisState state(moduleOp, options);
  if (failed(analyzeOp(moduleOp, state))) return signalPassFailure();

  // Rewrite init_tensors that are anchored on specific ops.
  IRRewriter rewriter(moduleOp->getContext());
  if (failed(bufferization::insertSliceAnchoredEmptyTensorEliminationStep(
          rewriter, moduleOp, state)))
    return signalPassFailure();
  if (failed(storeTensorOpAnchoredEmptyTensorEliminationStep(rewriter, moduleOp,
                                                             state)))
    return signalPassFailure();
}

// The following is copied from bufferization::runOneShotBufferize with
// modifications.
static LogicalResult runIREEOneShotBufferize(
    Operation *op, const OneShotBufferizationOptions &options) {
  OneShotAnalysisState state(op, options);
  if (failed(analyzeOp(op, state))) return failure();
  if (options.testAnalysisOnly) return success();
  return bufferization::runOneShotBufferize(op, options);
}

/// Run comprehensive bufferize.
void IREEComprehensiveBufferizePass::runOnOperation() {
  ModuleOp moduleOp = getOperation();
  OneShotBufferizationOptions options = getBufferizationOptions();
  options.testAnalysisOnly = testAnalysisOnly;
  options.printConflicts = printConflicts;
  options.allocationFn = allocationFn;
  options.deallocationFn = deallocationFn;
  options.memCpyFn = memCpyFn;

  if (failed(runIREEOneShotBufferize(moduleOp, options)))
    return signalPassFailure();
}

std::unique_ptr<OperationPass<ModuleOp>> createEliminateEmptyTensorsPass() {
  return std::make_unique<EliminateEmptyTensorsPass>();
}

std::unique_ptr<OperationPass<ModuleOp>> createIREEComprehensiveBufferizePass(
    Optional<BufferizationOptions::AllocationFn> allocationFn,
    Optional<BufferizationOptions::DeallocationFn> deallocationFn,
    Optional<BufferizationOptions::MemCpyFn> memCpyFn) {
  if (!allocationFn) allocationFn = defaultAllocationFn;
  if (!deallocationFn) deallocationFn = defaultDeallocationFn;
  if (!memCpyFn) memCpyFn = defaultMemCpyFn;
  return std::make_unique<IREEComprehensiveBufferizePass>(
      allocationFn, deallocationFn, memCpyFn);
}

void addIREEPostBufferizationPasses(OpPassManager &passManager) {
  passManager.addPass(memref::createResolveShapedTypeResultDimsPass());
  passManager.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  passManager.addNestedPass<func::FuncOp>(createCSEPass());
  // There are redundant memcpy (with linalg.generic form) ops created, which
  // can be deleted by canonicalizer. We have to run it again because the
  // memrefs are unified in CSE pass, so we can truely remove redundant memcpy.
  passManager.addNestedPass<func::FuncOp>(createCanonicalizerPass());
  passManager.addNestedPass<func::FuncOp>(createCleanupBufferAllocViewPass());
}

void addIREEComprehensiveBufferizePasses(
    OpPassManager &passManager,
    Optional<BufferizationOptions::AllocationFn> allocationFn,
    Optional<BufferizationOptions::DeallocationFn> deallocationFn,
    Optional<BufferizationOptions::MemCpyFn> memCpyFn) {
  passManager.addPass(createEliminateEmptyTensorsPass());
  passManager.addPass(bufferization::createEmptyTensorToAllocTensorPass());
  passManager.addPass(createIREEComprehensiveBufferizePass(
      allocationFn, deallocationFn, memCpyFn));
  addIREEPostBufferizationPasses(passManager);
}

}  // namespace iree_compiler
}  // namespace mlir
