// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_BUILTINS_UKERNEL_COMMON_H_
#define IREE_BUILTINS_UKERNEL_COMMON_H_

//===----------------------------------------------------------------------===//
// Generic microkernel library
//===----------------------------------------------------------------------===//
//
// Rules summary:
// 1. Microkernels are bare-metal, excluding even the standard C library.
//    a. Can't #include any system header.
//    b. Can't #include any standard library header.
//    c. Can't interface with the OS in any way.
// 2. Microkernels code may be specialized for a target CPU architecture, but
//    not for a complete target platform/OS/triple. In particular:
//    a. It's OK to have a `#ifdef __aarch64__` but not a `#ifdef __ANDROID__`.
// 3. Microkernels are pure/reentrant/stateless.
//    a. Pure: the only effect of calling a ukernel is to write to destination
//       buffers specified by pointers passed as ukernel arguments.
//    b. Reentrant: ukernels may be called concurrently with
//       themselves, other ukernels, or any other code, on any thread.
//    c. Stateless: ukernels can't mutate any global (or static local) variable.
//
// Explanation:
// 1. a. Microkernels will eventually be called from IREE LLVM-CPU codegen
//       modules. So we need to be able to build microkernels for all the target
//       architectures that iree-compile supports. If microkernels included
//       system headers, we would need to compile them not merely for each
//       target architecture but for each target triple, and we would need to
//       have the system headers for each of these.
// 1. b. Follows from a. because many standard C library headers #include
//       system headers. We can't keep track of which do. Even plausibly "pure"
//       ones such as <stdint.h> have been known to drag in surprising amounts.
// 1. c. Since we're only targeting a CPU architecture, not a complete target
//       platform/OS, we can't use any features that rely on the OS. For example
//       we can't use TLS (thread-local-storage) or Linux's auxiliary vector, or
//       syscalls.
//       * This means in particular that any CPU feature detection needs
//         to be made ahead of calling the ukernel, and the results passed as
//         ukernel args.
// 2. We don't want code to depend on platform `#ifdefs` beyond just target CPU
//    architecture ifdefs, in any way --- even if the code paths are not
//    interfacing with the OS (see 1.c.), it's still forbidden to have separate
//    code paths. When we will in the future call microkernels from IREE
//    LLVM-CPU codegen, this will make it legal for us to compile them only for
//    each target CPU architecture, which will be easier than having to compile
//    them separately for each supported target triple.
// 3. Microkernels are typically called on tiles, after the workload has been
//    tiled and distributed to several threads. Keeping microkernels pure,
//    reentrant and stateless keeps them automatically compatible with any
//    tiling and distribution that we may use in the future.
//
// FAQ:
// Q: Can a microkernel save, change, and restore the CPU float rounding mode?
//    A: Yes, as long as:
//       * It properly restores it in all its return paths.
//       * The CPU rounding mode is accessed in the microkernel's
//         own local code (as opposed to trying to use some standard library
//         header for that).
//       * The CPU architecture treats the rounding mode as a thread-local
//         setting (this tends to be the case on current CPU architectures).
// Q: How can a microkernel depend on CPU identification information?
//    A: Microkernels that need to know CPU identification information, such as
//       bits indicating support for optional SIMD ISA features, should take
//       such information as arguments. This moves the problem of obtaining the
//       CPU identification information to the caller. This serves multiple
//       purposes:
//       * This allows writing tests that exercise all variants supported by the
//         test machine, not just whichever variant would be selected for that
//         machine.
//       * On CPU architectures where only the OS can directly access CPU
//         identification bits (that includes ARM architectures), this is
//         basically required by rule 1.c. (forbidding microkernels from
//         querying the OS directly).
//         - While other CPU architectures like x86 allow userspace processes to
//           directly query CPU identification, it's best to keep all kernels
//           on all architectures aligned on this.
//         - While some OSes may trap CPU identification instructions to make
//           them appear as succeeding in userspace programs
//           (https://www.kernel.org/doc/html/latest/arm64/cpu-feature-registers.html),
//           there are portability, reliability and performance concerns with
//           that.

// Include the build-system-generated configured header and use it as the only
// source of information about the target we're compiling against, as opposed to
// including iree/base/target_platform.h.
//
// For example, using IREE_UK_ARCH_ARM_64 (from arch/config.h) rather than
// IREE_ARCH_ARM_64 (from target_platform.h) means that we can control from a
// single place in the build system whether we enable ARM_64-specific code paths
// or stick to generic code.
#include "iree/builtins/ukernel/arch/config.h"

// Include common flag values, shared with the compiler.
#include "iree/builtins/ukernel/exported_flag_bits.h"

// Include IREE_UK_STATIC_ASSERT.
#include "iree/builtins/ukernel/static_assert.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Attributes and metadata
//===----------------------------------------------------------------------===//

// Tagged on functions that are part of the public API.
// TODO(benvanik): use this to change symbol visibility? We don't want a library
// that embeds this one to transitively export functions but may want to have
// the functions exported based on how we link them. For now this is used as
// documentation.
#define IREE_UK_EXPORT

// Local fork of IREE_RESTRICT. We can't #include iree/base/attributes.h because
// it drags in platform headers, via target_platform.h. TODO, consider sharing
// this and other attributes that can be defined without any #include.
#if defined(_MSC_VER) && _MSC_VER >= 1900
#define IREE_UK_RESTRICT __restrict
#elif defined(_MSC_VER)
#define IREE_UK_RESTRICT
#elif defined(__cplusplus)
#define IREE_UK_RESTRICT __restrict__
#else
#define IREE_UK_RESTRICT restrict
#endif  // _MSC_VER

//===----------------------------------------------------------------------===//
// Local replacements for stdint.h types and constants
// Refer to the comment at the top of this file for why we can't include
// stdint.h.
//===----------------------------------------------------------------------===//

// These typedefs are making assumptions about the widths of standard C types.
// These assumptions are guarded by the IREE_UK_STATIC_ASSERT's below.
// If someday these assumptions fail, then we can always add #if's to control
// these typedefs, perhaps similarly to what is done for iree_uk_ssize_t
// below.
typedef signed char iree_uk_int8_t;
typedef short iree_uk_int16_t;
typedef int iree_uk_int32_t;
typedef long long iree_uk_int64_t;
typedef unsigned char iree_uk_uint8_t;
typedef unsigned short iree_uk_uint16_t;
typedef unsigned int iree_uk_uint32_t;
typedef unsigned long long iree_uk_uint64_t;

IREE_UK_STATIC_ASSERT(sizeof(iree_uk_int8_t) == 1);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_int16_t) == 2);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_int32_t) == 4);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_int64_t) == 8);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_uint8_t) == 1);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_uint16_t) == 2);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_uint32_t) == 4);
IREE_UK_STATIC_ASSERT(sizeof(iree_uk_uint64_t) == 8);

#define IREE_UK_INT8_MIN (-127i8 - 1)
#define IREE_UK_INT16_MIN (-32767i16 - 1)
#define IREE_UK_INT32_MIN (-2147483647i32 - 1)
#define IREE_UK_INT64_MIN (-9223372036854775807i64 - 1)
#define IREE_UK_INT8_MAX 127i8
#define IREE_UK_INT16_MAX 32767i16
#define IREE_UK_INT32_MAX 2147483647i32
#define IREE_UK_INT64_MAX 9223372036854775807i64
#define IREE_UK_UINT8_MAX 0xffui8
#define IREE_UK_UINT16_MAX 0xffffui16
#define IREE_UK_UINT32_MAX 0xffffffffui32
#define IREE_UK_UINT64_MAX 0xffffffffffffffffui64

// Helper for microkernel input validation
#define IREE_UK_VALUE_IN_UNSIGNED_INT_RANGE(VALUE, BIT_COUNT) \
  (((VALUE) >= 0) && !((VALUE) >> (BIT_COUNT)))

//===----------------------------------------------------------------------===//
// Local replacement for ssize_t
//===----------------------------------------------------------------------===//

// Use iree_uk_ssize_t for all sizes that may need pointer width.
// For any argument that is known to fit in a specific size prefer that to
// ensure this code operates well on systems with small/weird widths (x32/ilp32,
// etc).
#if IREE_UK_POINTER_SIZE == 4
typedef iree_uk_int32_t iree_uk_ssize_t;
#elif IREE_UK_POINTER_SIZE == 8
typedef iree_uk_int64_t iree_uk_ssize_t;
#else
#error Unexpected pointer size
#endif

static inline void iree_uk_ssize_swap(iree_uk_ssize_t* a, iree_uk_ssize_t* b) {
  iree_uk_ssize_t t = *a;
  *a = *b;
  *b = t;
}

//===----------------------------------------------------------------------===//
// Local replacement for stdbool.h
//===----------------------------------------------------------------------===//

#ifndef __cplusplus
// Exactly as in stdbool.h.
// As stdbool.h is only macros, not typedefs, and it is standardized how these
// macros expand, we can simply do them here. We still avoid #including it
// in case in some toolchain it might include unexpected other headers.
#define bool _Bool
#define true 1
#define false 0
#endif

//===----------------------------------------------------------------------===//
// Status codes returned by microkernels.
//===----------------------------------------------------------------------===//

typedef enum iree_uk_status_e {
  iree_uk_status_ok = 0,
  iree_uk_status_bad_type,
  iree_uk_status_bad_flags,
  iree_uk_status_unsupported_huge_or_negative_dimension,
  iree_uk_status_unsupported_generic_tile_size,
  iree_uk_status_shapes_mismatch,
} iree_uk_status_t;

// Convert a status code to a human-readable string.
IREE_UK_EXPORT const char* iree_uk_status_message(iree_uk_status_t status);

#define IREE_UK_RETURN_IF_ERROR(X)     \
  do {                                 \
    iree_uk_status_t status = (X);     \
    if (status != iree_uk_status_ok) { \
      return status;                   \
    }                                  \
  } while (0)

//===----------------------------------------------------------------------===//
// Element type IDs for the data accessed by microkernels.
//===----------------------------------------------------------------------===//

// Inspired by iree_hal_element_type_t, but more compact (8-bit instead of
// 32-bit), stand-alone (we couldn't use iree_hal_element_type_t at the moment
// anyway as that would #include more headers), and more specialized towards the
// subset of element types that we have in microkernels.
//
// The compactness is thought to be potentially valuable as many microkernels
// will have tuples of such element type ids and will perform if-else chains on
// the tuples, so if they can fit side-by-side in a single register, that will
// result in more compact code.
//
// Implementation note: we make this very bare-bones, with
// iree_uk_type_t just a typedef for iree_uk_uint8_t and
// the values given by macros, as opposed to trying to do something nicer, more
// strongly typed, etc, because of the following design goals:
// * Minimize divergence from iree_hal_element_type_t.
// * Minimize friction for microkernels authors. Examples:
//   * If people really care about writing switch statements as opposed to
//     if-else chains, it will be more convenient for them to have raw integers.
//     (C++ scoped enums would be perfect, but this is C code).
//   * If people ever need these type ids in assembly code, then the raw
//     numerical macros will be the only thing we'll be able to share with that
//     (as is the case today with  exported_flag_bits.h).

// Defines the element type of a buffer passed to a microkernel.
//
// Used as a bit-field. Current layout:
// * Bits 4..7 encode the 'category', e.g. integer or floating-point.
//   See IREE_UK_TYPE_CATEGORY_MASK.
// * Bit 3 is currently unused and reserved. It should always be set to 0.
// * Bit 0..2 encode the bit-count-log2, i.e. the bit width, required to be
//   a power of 2. See IREE_UK_TYPE_BIT_COUNT_LOG2_MASK.
typedef iree_uk_uint8_t iree_uk_type_t;

// Mask and bit values for the 'category' field within an element type.
// The general schema is that we use low values, from 1 upward, for integer-ish
// categories and high values, from 0xF downward, for floating-point-ish
// categories. This way, we simultaneously we keep it easy to implement the
// "is floating-point" test and we keep it open how many values will be used for
// integer-ish vs float-ish categories.
#define IREE_UK_TYPE_CATEGORY_MASK 0xF0u
// None-category, only used for the none-element-type (value 0).
#define IREE_UK_TYPE_CATEGORY_NONE 0x00u
// Opaque means that the values are just bits. Use in microkernel that only copy
// elements, and do not perform arithmetic on them.
#define IREE_UK_TYPE_CATEGORY_OPAQUE 0x10u
// Signless integers. Use in microkernels that perform same-bit-width integer
// arithmetic that is insensitive to signedness. For example, same-bit-width
// element-wise integer add and mul ops.
#define IREE_UK_TYPE_CATEGORY_INTEGER 0x20u
// Signed integers. Use in microkernels that are specifically performing signed
// integer arithmetic. For example, any mixed-bit-width op that involves a
// sign-extension (as in arith.extsi).
#define IREE_UK_TYPE_CATEGORY_INTEGER_SIGNED 0x30u
// Unsigned integers. Similar comments as for signed integers.
#define IREE_UK_TYPE_CATEGORY_INTEGER_UNSIGNED 0x40u
// "Brain" floating-point format. Currently only used for bfloat16.
#define IREE_UK_TYPE_CATEGORY_FLOAT_BRAIN 0xE0u
// IEEE754 floating-point format.
#define IREE_UK_TYPE_CATEGORY_FLOAT_IEEE 0xF0u

// Mask value for the 'bit-count-log2' field within an element type. 3 bits
// allow representing any power-of-two bit width from 1-bit to 128-bit, which
// matches what iree_hal_element_type_t can currently represent (as far as
// powers of two are concerned). If needed in the future, we could grow this
// by claiming the currently reserved bit 3.
#define IREE_UK_TYPE_BIT_COUNT_LOG2_MASK 0x07u

// Similar to iree_hal_element_types_t. We leave it a raw _e enum tag without a
// typedef because the enum type should never be used, only the enum values are
// expected to be used.
enum {
  IREE_UK_TYPE_NONE = IREE_UK_TYPE_CATEGORY_NONE | 0,
  IREE_UK_TYPE_OPAQUE_8 = IREE_UK_TYPE_CATEGORY_OPAQUE | 3,
  IREE_UK_TYPE_OPAQUE_16 = IREE_UK_TYPE_CATEGORY_OPAQUE | 4,
  IREE_UK_TYPE_OPAQUE_32 = IREE_UK_TYPE_CATEGORY_OPAQUE | 5,
  IREE_UK_TYPE_OPAQUE_64 = IREE_UK_TYPE_CATEGORY_OPAQUE | 6,
  IREE_UK_TYPE_INT_8 = IREE_UK_TYPE_CATEGORY_INTEGER | 3,
  IREE_UK_TYPE_INT_16 = IREE_UK_TYPE_CATEGORY_INTEGER | 4,
  IREE_UK_TYPE_INT_32 = IREE_UK_TYPE_CATEGORY_INTEGER | 5,
  IREE_UK_TYPE_INT_64 = IREE_UK_TYPE_CATEGORY_INTEGER | 6,
  IREE_UK_TYPE_SINT_8 = IREE_UK_TYPE_CATEGORY_INTEGER_SIGNED | 3,
  IREE_UK_TYPE_SINT_16 = IREE_UK_TYPE_CATEGORY_INTEGER_SIGNED | 4,
  IREE_UK_TYPE_SINT_32 = IREE_UK_TYPE_CATEGORY_INTEGER_SIGNED | 5,
  IREE_UK_TYPE_SINT_64 = IREE_UK_TYPE_CATEGORY_INTEGER_SIGNED | 6,
  IREE_UK_TYPE_UINT_8 = IREE_UK_TYPE_CATEGORY_INTEGER_UNSIGNED | 3,
  IREE_UK_TYPE_UINT_16 = IREE_UK_TYPE_CATEGORY_INTEGER_UNSIGNED | 4,
  IREE_UK_TYPE_UINT_32 = IREE_UK_TYPE_CATEGORY_INTEGER_UNSIGNED | 5,
  IREE_UK_TYPE_UINT_64 = IREE_UK_TYPE_CATEGORY_INTEGER_UNSIGNED | 6,
  IREE_UK_TYPE_FLOAT_16 = IREE_UK_TYPE_CATEGORY_FLOAT_IEEE | 4,
  IREE_UK_TYPE_FLOAT_32 = IREE_UK_TYPE_CATEGORY_FLOAT_IEEE | 5,
  IREE_UK_TYPE_FLOAT_64 = IREE_UK_TYPE_CATEGORY_FLOAT_IEEE | 6,
  IREE_UK_TYPE_BFLOAT_16 = IREE_UK_TYPE_CATEGORY_FLOAT_BRAIN | 4,
};

IREE_UK_STATIC_ASSERT(IREE_UK_TYPE_NONE == 0);

// Accessors.
static inline iree_uk_uint8_t iree_uk_type_category(iree_uk_type_t t) {
  return t & IREE_UK_TYPE_CATEGORY_MASK;
}

static inline int iree_uk_type_bit_count_log2(iree_uk_type_t t) {
  return t & IREE_UK_TYPE_BIT_COUNT_LOG2_MASK;
}

// Behavior is undefined if the bit-count is not a multiple of 8!
// The current implementation might return a negative value, but don't rely on
// that.
static inline int iree_uk_type_size_log2(iree_uk_type_t t) {
  return iree_uk_type_bit_count_log2(t) - 3;
}

static inline int iree_uk_type_bit_count(iree_uk_type_t t) {
  return 1 << iree_uk_type_bit_count_log2(t);
}

// Behavior is undefined if the bit-count is not a multiple of 8!
// Real C UB here (bit shift by negative amount), intentionally inviting the
// compiler to assume this can't happen.
static inline int iree_uk_type_size(iree_uk_type_t t) {
  return 1 << iree_uk_type_size_log2(t);
}

//===----------------------------------------------------------------------===//
// Tuples of types, packed into a word.
//===----------------------------------------------------------------------===//

typedef iree_uk_uint16_t iree_uk_type_pair_t;
typedef iree_uk_uint32_t iree_uk_type_triple_t;

#define IREE_UK_PACK_2_TYPES(B0, B1) ((B0) + ((B1) << 8))
#define IREE_UK_PACK_3_TYPES(B0, B1, B2) ((B0) + ((B1) << 8) + ((B2) << 16))
#define IREE_UK_PACK_2_TYPES_LITERAL(T0, T1) \
  IREE_UK_PACK_2_TYPES(IREE_UK_TYPE_##T0, IREE_UK_TYPE_##T1)
#define IREE_UK_PACK_3_TYPES_LITERAL(T0, T1, T2) \
  IREE_UK_PACK_3_TYPES(IREE_UK_TYPE_##T0, IREE_UK_TYPE_##T1, IREE_UK_TYPE_##T2)

#define IREE_UK_UNPACK_TYPE(POS, WORD) (((WORD) >> (8 * (POS))) & 0xFF)

static inline iree_uk_type_t iree_uk_unpack_type(int pos,
                                                 iree_uk_uint32_t word) {
  return IREE_UK_UNPACK_TYPE(pos, word);
}

#ifdef __has_builtin
#define IREE_UK_HAS_BUILTIN(x) __has_builtin(x)
#else
#define IREE_UK_HAS_BUILTIN(x) 0
#endif

// Same as LLVM_BUILTIN_UNREACHABLE. Extremely dangerous. Use only in locations
// that are provably unreachable (+/- edge case of unreachable-past-assertions
// discussed below).
//
// The potential benefit of UNREACHABLE statements is code size and/or speed
// optimization. This is an arcane optimization. As such, each use must be
// carefully justified.
//
// There is the edge case of locations that are provably unreachable when
// optional validation code is enabled, but the validation code may also be
// disabled, making the location technically reachable. Typically: assertions.
// Use careful judgement for such cases.
//
// A typical use case in microkernels is as follows. A microkernel is
// parametrized by type triples packed into uint32s, and needs to have a switch
// statement on those:
//
// switch (params->type_triple) {
//   case iree_uk_mykernel_f32f32f32:  // 0xf5f5f5
//     return 123;
//   case iree_uk_mykernel_i8i8i32:  // 0x232325
//     return 321;
//   default:
//     return 0;
// }
//
// As long as the microkernel has validation code (running at least as Debug
// assertions) validating type_triple, and this code is already past that,
// and this switch statement covers all valid cases, the `default:` case should
// be unreachable. Adding an UNREACHABLE statement there can help with code
// size. This would be negligible if the case constants were small enough to
// fit in compare-with-immediate instructions, but the 24-bit type triple
// constants here would typically not, so without UNREACHABLE, the compiler has
// to fully implement each 24-bit literal separately.
//
// https://godbolt.org/z/hTv4qqbx9 shows a snipped similar as above where
// the __builtin_unreachable shrinks the AArch64 code from 11 to 7 instructions.
#if IREE_UK_HAS_BUILTIN(__builtin_unreachable) || defined(__GNUC__)
#define IREE_UK_ASSUME_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define IREE_UK_ASSUME_UNREACHABLE __assume(false)
#else
#define IREE_UK_ASSUME_UNREACHABLE
#endif

// Queries for [[attribute]] identifiers in modern compilers.
#ifdef __has_attribute
#define IREE_UK_HAVE_ATTRIBUTE(x) __has_attribute(x)
#else
#define IREE_UK_HAVE_ATTRIBUTE(x) 0
#endif  // __has_attribute

#if IREE_UK_HAVE_ATTRIBUTE(noinline) || \
    (defined(__GNUC__) && !defined(__clang__))
#define IREE_UK_ATTRIBUTE_NOINLINE __attribute__((noinline))
#else
#define IREE_UK_ATTRIBUTE_NOINLINE
#endif  // IREE_UK_HAVE_ATTRIBUTE(noinline)

// The `restrict` here have the effect of enabling the compiler to rewrite this
// as a memcpy call, shrinking code size of the (slow anyway) generic code paths
// that would use this.
static inline void iree_uk_memcpy(void* IREE_UK_RESTRICT dst,
                                  const void* IREE_UK_RESTRICT src,
                                  iree_uk_ssize_t size) {
  for (iree_uk_ssize_t i = 0; i < size; ++i)
    ((char*)dst)[i] = ((const char*)src)[i];
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_BUILTINS_UKERNEL_COMMON_H_
