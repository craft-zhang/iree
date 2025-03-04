// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Util/IR/UtilOps.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Value.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Support/LogicalResult.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Util {

//===----------------------------------------------------------------------===//
// util.cmp.eq
//===----------------------------------------------------------------------===//

OpFoldResult CmpEQOp::fold(ArrayRef<Attribute> operands) {
  auto makeBool = [&](bool value) {
    return IntegerAttr::get(IntegerType::get(getContext(), 1), value ? 1 : 0);
  };
  if (getLhs() == getRhs()) {
    // SSA values are exactly the same.
    return makeBool(true);
  } else if (operands[0] && operands[1] && operands[0] == operands[1]) {
    // Folded attributes are equal but may come from separate ops.
    return makeBool(true);
  }
  // TODO(benvanik): we could add some interfaces for comparing, but this is
  // likely good enough for now.
  return {};
}

//===----------------------------------------------------------------------===//
// util.range.min/max
//===----------------------------------------------------------------------===//

static int64_t xmin(int64_t a, int64_t b) { return std::min(a, b); }
static int64_t xmax(int64_t a, int64_t b) { return std::max(a, b); }

template <int64_t initialValue, int64_t expr(int64_t, int64_t)>
static OpFoldResult foldRangeOp(Type type, ValueRange operands,
                                ArrayRef<Attribute> attrOperands) {
  // One operand is a pass-through.
  if (operands.size() == 1) {
    return operands.front();
  }

  // If all operands are constant then fold into a constant.
  int64_t value = initialValue;
  for (auto operand : attrOperands) {
    auto intValue = operand.dyn_cast_or_null<IntegerAttr>();
    if (!intValue) return {};
    value = expr(value, intValue.getValue().getSExtValue());
  }
  return IntegerAttr::get(type, value);
}

OpFoldResult RangeMinOp::fold(ArrayRef<Attribute> operands) {
  return foldRangeOp<INT64_MAX, xmin>(getType(), this->operands(), operands);
}

OpFoldResult RangeMaxOp::fold(ArrayRef<Attribute> operands) {
  return foldRangeOp<INT64_MIN, xmax>(getType(), this->operands(), operands);
}

namespace {

// Replaces util.range.min/max ops with the builtin min/max ops when possible.
//
// Example:
//  %min = util.range.min %0, %1 : index
// ->
//  %min = arith.minui %0, %1 : index
template <typename RangeOpT, typename StdOpT>
struct ExpandSimpleRangeOp : public OpRewritePattern<RangeOpT> {
  using OpRewritePattern<RangeOpT>::OpRewritePattern;
  LogicalResult matchAndRewrite(RangeOpT op,
                                PatternRewriter &rewriter) const override {
    if (op.getOperands().size() == 1) {
      rewriter.replaceOp(op, {op.getOperands().front()});
      return success();
    } else if (op.getOperands().size() == 2) {
      rewriter.replaceOpWithNewOp<StdOpT>(op, op.getOperands().front(),
                                          op.getOperands().back());
      return success();
    }
    return failure();
  }
};

// Simplifies min/max ops by folding constants and deduplicating values.
//
// Example:
//  %min = util.range.min %0, %c1, %c2, %0, %1
// ->
//  %min = util.range.min %c1, %0, %1
template <typename OpT, int64_t initialValue, int64_t expr(int64_t, int64_t)>
struct SimplifyUniformRangeOp : public OpRewritePattern<OpT> {
  using OpRewritePattern<OpT>::OpRewritePattern;
  LogicalResult matchAndRewrite(OpT op,
                                PatternRewriter &rewriter) const override {
    SetVector<Value> operands;
    int64_t constantValue = initialValue;
    for (auto operand : op.getOperands()) {
      APInt constantInt;
      if (matchPattern(operand, m_ConstantInt(&constantInt))) {
        // Constant value.
        constantValue = expr(constantValue, constantInt.getSExtValue());
      } else {
        // Dynamic value.
        operands.insert(operand);
      }
    }
    if (operands.size() + (constantValue != initialValue ? 1 : 0) ==
        op.operands().size()) {
      // No change in operand count.
      return failure();
    }
    if (constantValue != initialValue) {
      operands.insert(rewriter.create<arith::ConstantOp>(
          op.getLoc(),
          rewriter.getIntegerAttr(op.getResult().getType(), constantValue),
          op.getResult().getType()));
    }
    rewriter.replaceOpWithNewOp<OpT>(op, op.getResult().getType(),
                                     operands.takeVector());
    return success();
  }
};

}  // namespace

void RangeMinOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  results.insert<ExpandSimpleRangeOp<RangeMinOp, arith::MinUIOp>>(context);
  results.insert<SimplifyUniformRangeOp<RangeMinOp, INT64_MAX, xmin>>(context);
}

void RangeMaxOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  results.insert<ExpandSimpleRangeOp<RangeMaxOp, arith::MaxUIOp>>(context);
  results.insert<SimplifyUniformRangeOp<RangeMaxOp, INT64_MIN, xmax>>(context);
}

//===----------------------------------------------------------------------===//
// util.range.extents
//===----------------------------------------------------------------------===//

static Value makeRangeEnd(Location loc, Value offset, Value length, Value one,
                          OpBuilder &builder) {
  return builder.create<arith::SubIOp>(
      loc, builder.create<arith::AddIOp>(loc, offset, length), one);
}
static Value makeRangeEnd(Location loc, Value offset, Value length,
                          OpBuilder &builder) {
  return makeRangeEnd(
      loc, offset, length,
      builder.create<arith::ConstantOp>(
          loc, builder.getIntegerAttr(offset.getType(), 1), offset.getType()),
      builder);
}

namespace {

struct FoldConstantRanges : public OpRewritePattern<RangeExtentsOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(RangeExtentsOp op,
                                PatternRewriter &rewriter) const override {
    // Build a constant range for all we find and preserve the dynamic pairs.
    SmallVector<Value> offsets;
    SmallVector<Value> lengths;
    offsets.reserve(op.getOffsets().size());
    lengths.reserve(op.getLengths().size());
    int64_t constantMin = INT64_MAX;
    int64_t constantMax = INT64_MIN;
    for (auto range : llvm::zip(op.getOffsets(), op.getLengths())) {
      auto offset = std::get<0>(range);
      auto length = std::get<1>(range);
      APInt rangeOffset, rangeLength;
      if (matchPattern(offset, m_ConstantInt(&rangeOffset)) &&
          matchPattern(length, m_ConstantInt(&rangeLength))) {
        // Both offset and length are constant so we can fold.
        constantMin = std::min(constantMin, rangeOffset.getSExtValue());
        constantMax = std::max(constantMax,
                               (rangeOffset + rangeLength - 1).getSExtValue());
      } else {
        // Dynamic value that we'll preserve.
        offsets.push_back(offset);
        lengths.push_back(length);
      }
    }
    if (offsets.size() == op.getOffsets().size()) return failure();

    // Preserve dynamic ranges.
    Value min;
    Value max;
    if (!offsets.empty()) {
      auto newOp = rewriter.create<RangeExtentsOp>(
          op.getLoc(), op.getMin().getType(), op.getMax().getType(), offsets,
          lengths);
      min = newOp.getMin();
      max = newOp.getMax();
    }

    // Min/max with constant ranges. This allows for normal folding to happen
    // downstream of the op.
    auto constantMinOp = rewriter.create<arith::ConstantOp>(
        op.getLoc(),
        rewriter.getIntegerAttr(op.getMin().getType(), constantMin),
        op.getMin().getType());
    auto constantMaxOp = rewriter.create<arith::ConstantOp>(
        op.getLoc(),
        rewriter.getIntegerAttr(op.getMax().getType(),
                                constantMax - constantMin + 1),
        op.getMax().getType());
    min = min ? rewriter.create<arith::MinUIOp>(op.getLoc(), min, constantMinOp)
                    .getResult()
              : constantMinOp.getResult();
    max = max ? rewriter.create<arith::MaxUIOp>(op.getLoc(), max, constantMaxOp)
                    .getResult()
              : constantMaxOp.getResult();

    rewriter.replaceOp(op, {min, max});
    return success();
  }
};

struct ExpandSimpleRangeExtentsOp : public OpRewritePattern<RangeExtentsOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(RangeExtentsOp op,
                                PatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    Value minValue, maxValue;
    if (op.getOffsets().size() == 1) {
      // Single range folds to the min/max of that one range.
      minValue = op.getOffsets().front();
      maxValue = makeRangeEnd(loc, op.getOffsets().front(),
                              op.getLengths().front(), rewriter);
    } else if (op.getOffsets().size() == 2) {
      // Two ranges turn into min/max.
      minValue = rewriter.create<arith::MinUIOp>(loc, op.getOffsets().front(),
                                                 op.getOffsets().back());
      auto one = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getIntegerAttr(op.getMin().getType(), 1),
          op.getMin().getType());
      auto endLhs = makeRangeEnd(loc, op.getOffsets().front(),
                                 op.getLengths().front(), one, rewriter);
      auto endRhs = makeRangeEnd(loc, op.getOffsets().back(),
                                 op.getLengths().back(), one, rewriter);
      maxValue = rewriter.create<arith::MaxUIOp>(loc, endLhs, endRhs);
    }
    if (!minValue || !maxValue) return failure();
    rewriter.replaceOp(op, {minValue, maxValue});
    return success();
  }
};

struct DeduplicateRangeExtentsOp : public OpRewritePattern<RangeExtentsOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(RangeExtentsOp op,
                                PatternRewriter &rewriter) const override {
    // First filter out any pure duplicates. Note SetVector so order is
    // preserved.
    using Range = std::tuple<Value, Value>;
    SetVector<Range> ranges;
    for (auto range : llvm::zip(op.getOffsets(), op.getLengths())) {
      ranges.insert(range);
    }
    if (ranges.size() == op.getOffsets().size()) return failure();

    // Recreate with the deduplicated ranges.
    SmallVector<Value> offsets;
    SmallVector<Value> lengths;
    offsets.reserve(ranges.size());
    lengths.reserve(ranges.size());
    for (auto range : llvm::enumerate(ranges)) {
      offsets.push_back(std::get<0>(range.value()));
      lengths.push_back(std::get<1>(range.value()));
    }
    rewriter.replaceOpWithNewOp<RangeExtentsOp>(
        op, op.getMin().getType(), op.getMax().getType(), offsets, lengths);
    return success();
  }
};

}  // namespace

void RangeExtentsOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  // TODO(benvanik): extract ranges with common offsets or lengths and move them
  // to min/max ops where they have a better chance of folding.
  results.insert<FoldConstantRanges>(context);
  results.insert<ExpandSimpleRangeExtentsOp>(context);
  results.insert<DeduplicateRangeExtentsOp>(context);
}

//===----------------------------------------------------------------------===//
// util.align
//===----------------------------------------------------------------------===//

// TODO(#5405): add canonicalizers that reach further in the IR or a dedicated
// pass for full potential-value-set analysis.

// Returns true if |value| is definitely aligned to at least |alignment|.
// Recursively checks up the source of the value to see if we can trivially
// prove the alignment either directly matches (when dynamic) or is >= the
// specified |alignment|. This does not walk across blocks or calls but catches
// a large majority of the cases we generate ourselves from packing/allocation.
static bool isAlignedTo(Value value, Value alignment) {
  APInt staticValue;
  APInt staticAlignment;
  if (matchPattern(value, m_ConstantInt(&staticValue)) &&
      matchPattern(alignment, m_ConstantInt(&staticAlignment))) {
    // If this value is itself a multiple of the alignment then we can fold.
    if (staticValue.urem(staticAlignment).isZero()) {
      return true;  // value % alignment == 0
    }
  }

  // If the value is produced by an align op we can check that.
  if (auto sourceAlignOp = value.getDefiningOp<IREE::Util::AlignOp>()) {
    // Check for same exact alignment - even if dynamic.
    if (sourceAlignOp.getAlignment() == alignment) return true;

    // If the alignments are constant we can compare them inline.
    APInt sourceAlignment;
    APInt selfAlignment;
    if (matchPattern(sourceAlignOp.getAlignment(),
                     m_ConstantInt(&sourceAlignment)) &&
        matchPattern(alignment, m_ConstantInt(&selfAlignment))) {
      if (sourceAlignment.uge(selfAlignment)) {
        return true;  // source alignment is >= our alignment
      }
    }

    // Recurse and check the alignment on the input to the align; if it was
    // aligned earlier we can rely on that as align will never shrink a value.
    return isAlignedTo(sourceAlignOp.getValue(), alignment);
  }

  // If we are sourced from add/mul we peephole check to see if what is being
  // added is also aligned. This should be part of a larger pass doing IPO but
  // as the common case is that we align+add+align this is worth having in a
  // folder. This single folder can avoid ever even materializing thousands of
  // ops.
  if (auto sourceAddOp = value.getDefiningOp<arith::AddIOp>()) {
    // Two aligned values added together are still aligned.
    if (isAlignedTo(sourceAddOp.getLhs(), alignment) &&
        isAlignedTo(sourceAddOp.getRhs(), alignment)) {
      return true;
    }
  } else if (auto sourceSubOp = value.getDefiningOp<arith::SubIOp>()) {
    // An aligned value subtracted from an aligned value is still aligned.
    if (isAlignedTo(sourceSubOp.getLhs(), alignment) &&
        isAlignedTo(sourceSubOp.getRhs(), alignment)) {
      return true;
    }
  } else if (auto sourceMulOp = value.getDefiningOp<arith::MulIOp>()) {
    // Two aligned values multiplied together are still aligned.
    if (isAlignedTo(sourceMulOp.getLhs(), alignment) &&
        isAlignedTo(sourceMulOp.getRhs(), alignment)) {
      return true;
    }
  }

  return false;
}

OpFoldResult AlignOp::fold(ArrayRef<Attribute> operands) {
  // If aligning an already-aligned value then fold if this is provably a
  // no-op. We can check this for equality even with dynamic alignments.
  if (isAlignedTo(getValue(), getAlignment())) return getValue();
  return {};
}

//===----------------------------------------------------------------------===//
// util.sizeof
//===----------------------------------------------------------------------===//

OpFoldResult SizeOfOp::fold(ArrayRef<Attribute> operands) {
  Type t = getSizedType();
  if (t.isa<IntegerType>() || t.isa<FloatType>()) {
    return IntegerAttr::get(IndexType::get(getContext()),
                            getRoundedElementByteWidth(t));
  }
  return {};
}

//===----------------------------------------------------------------------===//
// Compiler hints
//===----------------------------------------------------------------------===//

namespace {

struct ExpandUnfoldableConstantOp
    : public OpRewritePattern<UnfoldableConstantOp> {
  using OpRewritePattern<IREE::Util::UnfoldableConstantOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(UnfoldableConstantOp op,
                                PatternRewriter &rewriter) const override {
    auto stdConst =
        rewriter.create<arith::ConstantOp>(op.getLoc(), op.getValue());
    rewriter.replaceOpWithNewOp<OptimizationBarrierOp>(op,
                                                       stdConst.getResult());
    return success();
  }
};

}  // namespace

void UnfoldableConstantOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.insert<ExpandUnfoldableConstantOp>(context);
}

//===----------------------------------------------------------------------===//
// Globals
//===----------------------------------------------------------------------===//

namespace {

// Deletes empty vm.initializer ops.
struct DropEmptyInitializerOp : public OpRewritePattern<InitializerOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InitializerOp op,
                                PatternRewriter &rewriter) const override {
    if (op.getBody().getBlocks().size() != 1) return failure();
    auto &block = op.getBody().front();
    if (block.empty() || isa<InitializerReturnOp>(block.front())) {
      rewriter.eraseOp(op);
      return success();
    }
    return failure();
  }
};

// Inlines constant stores from initializers into the global initializer.
// This is not strictly required but can help our initialization code perform
// more efficient initialization of large numbers of primitive values.
struct InlineConstantGlobalInitializer
    : public OpRewritePattern<InitializerOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InitializerOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Operation *> deadOps;
    op.walk([&](GlobalStoreOpInterface storeOp) {
      Attribute valueAttr;
      if (!matchPattern(storeOp.getStoredGlobalValue(),
                        m_Constant(&valueAttr))) {
        return;
      }
      auto globalOp =
          SymbolTable::lookupNearestSymbolFrom<IREE::Util::GlobalOpInterface>(
              storeOp->getParentOp(), storeOp.getGlobalAttr());
      rewriter.updateRootInPlace(
          globalOp, [&]() { globalOp.setGlobalInitialValue(valueAttr); });

      deadOps.push_back(storeOp);
    });
    if (deadOps.empty()) return failure();
    for (auto deadOp : deadOps) rewriter.eraseOp(deadOp);
    return success();
  }
};

}  // namespace

void InitializerOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.insert<DropEmptyInitializerOp, InlineConstantGlobalInitializer>(
      context);
}

void GlobalOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {}

namespace {

/// Turns util.global.address -> util.global.load.indirect into a direct load.
class PropagateGlobalLoadAddress
    : public OpRewritePattern<GlobalLoadIndirectOp> {
  using OpRewritePattern::OpRewritePattern;

 public:
  LogicalResult matchAndRewrite(GlobalLoadIndirectOp op,
                                PatternRewriter &rewriter) const override {
    if (auto addressOp = dyn_cast_or_null<GlobalAddressOpInterface>(
            op.getGlobal().getDefiningOp())) {
      rewriter.replaceOpWithNewOp<GlobalLoadOp>(op, op.getResult().getType(),
                                                addressOp.getGlobalAttr());
      return success();
    }
    return failure();
  }
};

}  // namespace

void GlobalLoadIndirectOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.insert<PropagateGlobalLoadAddress>(context);
}

namespace {

/// Erases util.global.store ops that are no-ops.
/// This can happen if there was a global load, some DCE'd usage, and a
/// store back to the same global: we want to be able to elide the entire load
/// and store.
struct EraseUnusedGlobalStoreOp : public OpRewritePattern<GlobalStoreOp> {
  using OpRewritePattern<GlobalStoreOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(GlobalStoreOp op,
                                PatternRewriter &rewriter) const override {
    if (auto loadOp = dyn_cast_or_null<GlobalLoadOpInterface>(
            op.getValue().getDefiningOp())) {
      if (loadOp.getGlobalName() == op.getGlobal()) {
        rewriter.eraseOp(op);
        return success();
      }
    }
    return failure();
  }
};

}  // namespace

void GlobalStoreOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.insert<EraseUnusedGlobalStoreOp>(context);
}

namespace {

/// Turns util.global.address -> util.global.store.indirect into a direct store.
class PropagateGlobalStoreAddress
    : public OpRewritePattern<GlobalStoreIndirectOp> {
  using OpRewritePattern::OpRewritePattern;

 public:
  LogicalResult matchAndRewrite(GlobalStoreIndirectOp op,
                                PatternRewriter &rewriter) const override {
    if (auto addressOp = dyn_cast_or_null<GlobalAddressOpInterface>(
            op.getGlobal().getDefiningOp())) {
      rewriter.replaceOpWithNewOp<GlobalStoreOp>(op, op.getValue(),
                                                 addressOp.getGlobalAttr());
      return success();
    }
    return failure();
  }
};

}  // namespace

void GlobalStoreIndirectOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.insert<PropagateGlobalStoreAddress>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.alloc
//===----------------------------------------------------------------------===//

void BufferAllocOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  // TODO(benvanik): elide if only users are writes and dealloc.
}

//===----------------------------------------------------------------------===//
// util.buffer.slice
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan ranges into slice ranges.
//
// Example:
//  %0 = util.buffer.subspan %src[%subspan_offset] ... -> {%subspan_length}
//  %1 = util.buffer.slice %0[%slice_offset] ... -> {%slice_length}
// ->
//  %new_offset = arith.addi %slice_offset, %subspan_offset
//  %1 = util.buffer.slice %src[%new_offset] ... -> {%slice_length}
struct FoldSubspansIntoSliceOp : public OpRewritePattern<BufferSliceOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferSliceOp op,
                                PatternRewriter &rewriter) const override {
    auto subspanOp = BufferSubspanOp::findSubspanOp(op.getSource());
    if (!subspanOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subspanOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, subspanOp.getSourceOffset(), op.getSourceOffset());
    rewriter.updateRootInPlace(op, [&]() {
      op.getSourceMutable().assign(subspanOp.getSource());
      op.getSourceSizeMutable().assign(subspanOp.getSourceSize());
      op.getSourceOffsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void BufferSliceOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.insert<FoldSubspansIntoSliceOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.subspan
//===----------------------------------------------------------------------===//

OpFoldResult BufferSubspanOp::fold(ArrayRef<Attribute> operands) {
  if (getSourceSize() == getResultSize()) {
    // Entire range is covered; return it all.
    return getSource();
  }
  return {};
}

namespace {

// Folds subspan -> subspan to point at the original source buffer with an
// updated range.
struct FoldBufferSubspanOps : public OpRewritePattern<BufferSubspanOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferSubspanOp op,
                                PatternRewriter &rewriter) const override {
    auto parentOp = BufferSubspanOp::findSubspanOp(op.getSource());
    if (!parentOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({parentOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, parentOp.getSourceOffset(), op.getSourceOffset());
    auto newOp = rewriter.create<BufferSubspanOp>(
        fusedLoc, parentOp.getSource(), parentOp.getSourceSize(), newOffset,
        op.getResultSize());
    rewriter.replaceOp(op, newOp.getResult());
    return success();
  }
};

// Turns selects of subspans of a buffer into selects of the offset.
// This only works if the subspan sizes match.
//
// Example:
//  %subspan0 = util.buffer.subspan %src[%offset0]
//  %subspan1 = util.buffer.subspan %src[%offset1]
//  %subspan = select %cond, %subspan0, %subspan1 : !util.buffer
// ->
//  %offset = select %cond, %offset0, %offset1 : index
//  %subspan = util.buffer.subspan %src[%offset]
struct SinkSubspanAcrossSelectOps
    : public OpRewritePattern<mlir::arith::SelectOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(mlir::arith::SelectOp op,
                                PatternRewriter &rewriter) const override {
    if (!op.getType().isa<IREE::Util::BufferType>()) return failure();
    auto trueSubspan = dyn_cast_or_null<IREE::Util::BufferSubspanOp>(
        op.getTrueValue().getDefiningOp());
    auto falseSubspan = dyn_cast_or_null<IREE::Util::BufferSubspanOp>(
        op.getFalseValue().getDefiningOp());
    if (!trueSubspan || !falseSubspan) return failure();
    if (trueSubspan.getSource() != falseSubspan.getSource() ||
        trueSubspan.getResultSize() != falseSubspan.getResultSize()) {
      return failure();
    }
    auto offsetSelectOp = rewriter.create<mlir::arith::SelectOp>(
        op.getLoc(), op.getCondition(), trueSubspan.getSourceOffset(),
        falseSubspan.getSourceOffset());
    rewriter.replaceOpWithNewOp<IREE::Util::BufferSubspanOp>(
        op, op.getResult().getType(), trueSubspan.getSource(),
        trueSubspan.getSourceSize(), offsetSelectOp.getResult(),
        trueSubspan.getResultSize());
    return success();
  }
};

}  // namespace

void BufferSubspanOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.insert<FoldBufferSubspanOps>(context);
  results.insert<SinkSubspanAcrossSelectOps>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.size
//===----------------------------------------------------------------------===//

OpFoldResult BufferSizeOp::fold(ArrayRef<Attribute> operands) {
  // Try to find the size in the use-def chain.
  // If it's out of the local scope we'll need IPO to help out.
  // During A->B->C dialect conversion, the type may not be legal so be
  // defensive.
  auto operand = getOperand();
  if (auto sizeAwareType =
          operand.getType().dyn_cast<IREE::Util::SizeAwareTypeInterface>()) {
    Operation *op = this->getOperation();
    if (auto sizeValue = sizeAwareType.findSizeValue(operand, op->getBlock(),
                                                     Block::iterator(op))) {
      return sizeValue;
    }
  }

  // If the source is a constant then we can calculate that immediately.
  if (auto constantOp = dyn_cast_or_null<IREE::Util::BufferConstantOp>(
          operand.getDefiningOp())) {
    if (auto attr =
            constantOp.getValue()
                .dyn_cast_or_null<IREE::Util::SerializableAttrInterface>()) {
      return IntegerAttr::get(IndexType::get(attr.getContext()),
                              attr.getStorageSize());
    }
  }

  return {};
}

namespace {

// Propagates buffer sizes through select ops by selecting on the sizes of the
// select operands.
//
// Example:
//  %a = util.buffer... : !util.buffer{%a_sz}
//  %b = util.buffer... : !util.buffer{%b_sz}
//  %c = select %cond, %a, %b : !util.buffer
//  %c_sz = util.buffer.size %c : !util.buffer
// ->
//  %c = select %cond, %a, %b : !util.buffer
//  %c_sz = select %cond, %a_sz, %b_sz : index
struct SelectBufferSizeOp : public OpRewritePattern<BufferSizeOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferSizeOp op,
                                PatternRewriter &rewriter) const override {
    auto selectOp = op.getOperand().getDefiningOp<mlir::arith::SelectOp>();
    if (!selectOp) return failure();
    auto trueSize = rewriter.createOrFold<IREE::Util::BufferSizeOp>(
        op.getLoc(), selectOp.getTrueValue());
    auto falseSize = rewriter.createOrFold<IREE::Util::BufferSizeOp>(
        op.getLoc(), selectOp.getFalseValue());
    rewriter.replaceOpWithNewOp<mlir::arith::SelectOp>(
        op, selectOp.getCondition(), trueSize, falseSize);
    return success();
  }
};

}  // namespace

void BufferSizeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.insert<SelectBufferSizeOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.storage
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan ranges into storage ranges.
//
// Example:
//  %0 = util.buffer.subspan %src[%subspan_offset] ... -> {%subspan_length}
//  %storage, %offset = util.buffer.storage %0
// ->
//  %storage, %raw_offset = util.buffer.storage %src
//  %offset = arith.addi %raw_offset, %subspan_offset
struct FoldSubspansIntoStorageOp : public OpRewritePattern<BufferStorageOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferStorageOp op,
                                PatternRewriter &rewriter) const override {
    auto subspanOp = BufferSubspanOp::findSubspanOp(op.getOperand());
    if (!subspanOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subspanOp.getLoc(), op.getLoc()});
    rewriter.setInsertionPointAfter(op);
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, subspanOp.getSourceOffset(), op.getOffset());
    rewriter.updateRootInPlace(op, [&]() {
      op.getOperandMutable().assign(subspanOp.getSource());
      op.getOperandSizeMutable().assign(subspanOp.getSourceSize());
      SmallPtrSet<Operation *, 2> exceptions;
      exceptions.insert(op);
      if (auto newOffsetOp = newOffset.getDefiningOp()) {
        exceptions.insert(newOffsetOp);
      }
      op.getOffset().replaceAllUsesExcept(newOffset, exceptions);
    });
    return success();
  }
};

}  // namespace

void BufferStorageOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.insert<FoldSubspansIntoStorageOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.copy
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan ranges into copy ranges.
//
// Example:
//  %0 = util.buffer.subspan %src[%subspan_offset] ... -> {%subspan_length}
//  %1 = util.buffer.subspan %dst[%subspan_offset] ... -> {%subspan_length}
//  util.buffer.copy %0[%offset], %1[%offset], %length
// ->
//  %new_offset = arith.addi %offset, %subspan_offset
//  util.buffer.copy %src[%new_offset], %dst[%new_offset], %subspan_length
struct FoldSubspansIntoCopyOp : public OpRewritePattern<BufferCopyOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferCopyOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceSubspanOp = BufferSubspanOp::findSubspanOp(op.getSource());
    auto targetSubspanOp = BufferSubspanOp::findSubspanOp(op.getTarget());
    if (!sourceSubspanOp && !targetSubspanOp) return failure();
    if (sourceSubspanOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({sourceSubspanOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<arith::AddIOp>(
          fusedLoc, sourceSubspanOp.getSourceOffset(), op.getSourceOffset());
      rewriter.updateRootInPlace(op, [&]() {
        op.getSourceMutable().assign(sourceSubspanOp.getSource());
        op.getSourceSizeMutable().assign(sourceSubspanOp.getSourceSize());
        op.getSourceOffsetMutable().assign(newOffset);
      });
    }
    if (targetSubspanOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({targetSubspanOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<arith::AddIOp>(
          fusedLoc, targetSubspanOp.getSourceOffset(), op.getTargetOffset());
      rewriter.updateRootInPlace(op, [&]() {
        op.getTargetMutable().assign(targetSubspanOp.getSource());
        op.getTargetSizeMutable().assign(targetSubspanOp.getSourceSize());
        op.getTargetOffsetMutable().assign(newOffset);
      });
    }
    return success();
  }
};

}  // namespace

void BufferCopyOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.insert<FoldSubspansIntoCopyOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.compare
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan ranges into copy ranges.
//
// Example:
//  %0 = util.buffer.subspan %src[%subspan_offset] ... -> {%subspan_length}
//  %1 = util.buffer.subspan %dst[%subspan_offset] ... -> {%subspan_length}
//  util.buffer.copy %0[%offset], %1[%offset], %length
// ->
//  %new_offset = arith.addi %offset, %subspan_offset
//  util.buffer.copy %src[%new_offset], %dst[%new_offset], %subspan_length
struct FoldSubspansIntoCompareOp : public OpRewritePattern<BufferCompareOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferCompareOp op,
                                PatternRewriter &rewriter) const override {
    auto sourceSubspanOp = BufferSubspanOp::findSubspanOp(op.getLhs());
    auto targetSubspanOp = BufferSubspanOp::findSubspanOp(op.getRhs());
    if (!sourceSubspanOp && !targetSubspanOp) return failure();
    if (sourceSubspanOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({sourceSubspanOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<arith::AddIOp>(
          fusedLoc, sourceSubspanOp.getSourceOffset(), op.getLhsOffset());
      rewriter.updateRootInPlace(op, [&]() {
        op.getLhsMutable().assign(sourceSubspanOp.getSource());
        op.getLhsSizeMutable().assign(sourceSubspanOp.getSourceSize());
        op.getLhsOffsetMutable().assign(newOffset);
      });
    }
    if (targetSubspanOp) {
      auto fusedLoc =
          rewriter.getFusedLoc({targetSubspanOp.getLoc(), op.getLoc()});
      auto newOffset = rewriter.createOrFold<arith::AddIOp>(
          fusedLoc, targetSubspanOp.getSourceOffset(), op.getRhsOffset());
      rewriter.updateRootInPlace(op, [&]() {
        op.getRhsMutable().assign(targetSubspanOp.getSource());
        op.getRhsSizeMutable().assign(targetSubspanOp.getSourceSize());
        op.getRhsOffsetMutable().assign(newOffset);
      });
    }
    return success();
  }
};

}  // namespace

void BufferCompareOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.insert<FoldSubspansIntoCompareOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.fill
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan ranges into fill ranges.
//
// Example:
//  %0 = util.buffer.subspan %dst[%subspan_offset] ... -> {%subspan_length}
//  util.buffer.fill %cst, %0[%offset for %length]
// ->
//  %new_offset = arith.addi %offset, %subspan_offset
//  util.buffer.fill %cst, %dst[%new_offset for %subspan_length]
struct FoldSubspansIntoFillOp : public OpRewritePattern<BufferFillOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferFillOp op,
                                PatternRewriter &rewriter) const override {
    auto subspanOp = BufferSubspanOp::findSubspanOp(op.getTarget());
    if (!subspanOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subspanOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, subspanOp.getSourceOffset(), op.getTargetOffset());
    rewriter.updateRootInPlace(op, [&]() {
      op.getTargetMutable().assign(subspanOp.getSource());
      op.getTargetSizeMutable().assign(subspanOp.getSourceSize());
      op.getTargetOffsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void BufferFillOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.insert<FoldSubspansIntoFillOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.load
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan offsets into loads.
//
// Example:
//  %0 = util.buffer.subspan %src[%subspan_offset] ... -> {%subspan_length}
//  %1 = util.buffer.load %0[%offset]
// ->
//  %new_offset = arith.addi %offset, %subspan_offset
//  %1 = util.buffer.load %src[%new_offset]
struct FoldSubspanIntoLoadOp : public OpRewritePattern<BufferLoadOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferLoadOp op,
                                PatternRewriter &rewriter) const override {
    auto subspanOp = BufferSubspanOp::findSubspanOp(op.getSource());
    if (!subspanOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subspanOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, subspanOp.getSourceOffset(), op.getSourceOffset());
    rewriter.updateRootInPlace(op, [&]() {
      op.getSourceMutable().assign(subspanOp.getSource());
      op.getSourceSizeMutable().assign(subspanOp.getSourceSize());
      op.getSourceOffsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

OpFoldResult BufferLoadOp::fold(ArrayRef<Attribute> operands) {
  // TODO(benvanik): if source is a constant then perform the load.
  return {};
}

void BufferLoadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.insert<FoldSubspanIntoLoadOp>(context);
}

//===----------------------------------------------------------------------===//
// util.buffer.store
//===----------------------------------------------------------------------===//

namespace {

// Folds subspan offsets into stores.
//
// Example:
//  %0 = util.buffer.subspan %dst[%subspan_offset] ... -> {%subspan_length}
//  util.buffer.store %c123_i32, %0[%offset]
// ->
//  %new_offset = arith.addi %offset, %subspan_offset
//  util.buffer.store %c123_i32, %dst[%new_offset]
struct FoldSubspanIntoStoreOp : public OpRewritePattern<BufferStoreOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(BufferStoreOp op,
                                PatternRewriter &rewriter) const override {
    auto subspanOp = BufferSubspanOp::findSubspanOp(op.getTarget());
    if (!subspanOp) return failure();
    auto fusedLoc = rewriter.getFusedLoc({subspanOp.getLoc(), op.getLoc()});
    auto newOffset = rewriter.createOrFold<arith::AddIOp>(
        fusedLoc, subspanOp.getSourceOffset(), op.getTargetOffset());
    rewriter.updateRootInPlace(op, [&]() {
      op.getTargetMutable().assign(subspanOp.getSource());
      op.getTargetSizeMutable().assign(subspanOp.getSourceSize());
      op.getTargetOffsetMutable().assign(newOffset);
    });
    return success();
  }
};

}  // namespace

void BufferStoreOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.insert<FoldSubspanIntoStoreOp>(context);
}

}  // namespace Util
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
