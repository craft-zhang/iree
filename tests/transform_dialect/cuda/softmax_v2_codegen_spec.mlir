// RUN: iree-opt %s

// Codegen
transform.structured.canonicalized_sequence failures(propagate) {
^bb1(%variant_op: !pdl.operation):
  %ops = transform.structured.match ops{["linalg.fill", "linalg.generic"]}
    in %variant_op
  %input_max_fill,
  %input_max,
  %exps_sum_fill,
  %exp_and_exps_sum,
  %div = transform.split_handles %ops in [5]
    : (!pdl.operation) -> (!pdl.operation, !pdl.operation, !pdl.operation,
                           !pdl.operation, !pdl.operation)

  // Step 1. First level of tiling + fusion parallelizes to blocks.
  // ==============================================================
  %foreach_thread, %_ =
  transform.iree.tile_to_foreach_thread_and_workgroup_count_region %div tile_sizes [1, 4]  
    ( mapping = [#gpu.block<x>, #gpu.block<y>] )
  // TODO: Merging and fusing merged handles does not work properly atm.
  transform.structured.fuse_into_containing_op %exp_and_exps_sum into %foreach_thread
  transform.structured.fuse_into_containing_op %exps_sum_fill into %foreach_thread
  transform.structured.fuse_into_containing_op %input_max into %foreach_thread
  transform.structured.fuse_into_containing_op %input_max_fill into %foreach_thread
  // By default, fusion into scf.foreach_thread does not promote captured values
  // to shared as this involves a cross-thread dependence analysis.
  // Instead, we activate it explicitly post-hoc to promote all the extract_slice
  // ops that we find and match the prerequisites
  %func = transform.structured.match ops{["func.func"]} in %variant_op
  %funcx = transform.iree.apply_patterns %func { promote_foreach_thread_capture_to_shared }

  // Step 2. Second level of tiling + fusion parallelizes to threads.
  // ================================================================
  %tiled_ops = transform.structured.match ops{["linalg.fill", "linalg.generic"]}
    in %variant_op
  %tiled_input_max_fill,
  %tiled_input_max,
  %tiled_exps_sum_fill,
  %tiled_exp_and_exps_sum,
  %tiled_div = transform.split_handles %tiled_ops in [5]
    : (!pdl.operation) -> (!pdl.operation, !pdl.operation, !pdl.operation,
                           !pdl.operation, !pdl.operation)
  // Leaving the reduction untiled on threadIdx.x makes it sequential on
  // threadIdx.x. After distribution, predication by `if (threadIdx.x == 0)` is
  // introduced and opportunities for distributing vector ops across warps
  // appear.
  %reduction_linalg_ops = transform.merge_handles %tiled_input_max,
                                                  %tiled_exp_and_exps_sum
    : !pdl.operation
  transform.structured.tile_to_foreach_thread_op %reduction_linalg_ops tile_sizes [1, 1]
    ( mapping = [#gpu.thread<z>, #gpu.thread<y>] )
  // Fully parallel ops are tiled and mapped.
  %parallel_linalg_ops = transform.merge_handles %tiled_input_max_fill,
                                                 %tiled_exps_sum_fill,
                                                 %tiled_div
    : !pdl.operation
  transform.structured.tile_to_foreach_thread_op %parallel_linalg_ops num_threads [1, 4, 32]
    ( mapping = [#gpu.thread<z>, #gpu.thread<y>, #gpu.thread<x>] )
  // Step 3. Rank-reduce and vectorize.
  // ==================================
  %funcx_2 = transform.structured.match ops{["func.func"]} in %variant_op
  %funcx_3 = transform.iree.apply_patterns %funcx_2 { rank_reducing }
  transform.structured.vectorize %funcx_3

  // Step 4. Bufferize.
  // ==================
  %variant_op_2 = transform.iree.bufferize { target_gpu } %variant_op

  // Step 5. Post-bufferization mapping to blocks and threads.
  // =========================================================
  %func_2 = transform.structured.match ops{["func.func"]} in %variant_op_2
  %func_3 = transform.iree.foreach_thread_to_workgroup %func_2
  transform.iree.map_nested_foreach_thread_to_gpu_threads %func_3
    { workgroup_size = [32, 4, 1] }

  // Step 6. Post-bufferization vector distribution with rank-reduction.
  // ===================================================================
  %end_func = transform.structured.match ops{["func.func"]} in %variant_op_2
  %end_func_2 = transform.iree.apply_patterns %end_func { rank_reducing }
  %if_op = transform.structured.match ops{["scf.if"]} in %variant_op_2
  %warp = transform.iree.vector.to_warp_execute_on_lane_0 %if_op { warp_size = 32 }
  transform.iree.vector.warp_distribute %end_func_2
}
