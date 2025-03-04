// RUN: iree-opt %s

transform.structured.canonicalized_sequence failures(suppress) {
^bb1(%variant_op: !pdl.operation):
  %fill = transform.structured.match ops{["linalg.fill"]} in %variant_op

  // Step 1. Split the reduction to get meatier (size(red) / 2)-way parallelism.
  // ===========================================================================
  %0 = transform.structured.match ops{["linalg.generic"]} in %variant_op
  %init_or_alloc_op, %more_parallel_fill_op, %more_parallel_op, %combiner_op =
    transform.structured.split_reduction %0
      { split_factor = 2, insert_split_dimension = 1 }

  // Step 2. First level of tiling + fusion parallelizes to blocks.
  // ===========================================================================
  %foreach_thread_grid, %grid_combiner_op =
    transform.iree.tile_to_foreach_thread_and_workgroup_count_region %combiner_op tile_sizes [1]
      ( mapping = [#gpu.block<x>] )
  %not_combiner = transform.merge_handles %fill, %more_parallel_fill_op, %more_parallel_op : !pdl.operation
  transform.structured.fuse_into_containing_op %not_combiner into %foreach_thread_grid

  // Step 3. Second level of tiling + fusion parallelizes to threads.
  // ===========================================================================
  %fill_1d = transform.structured.match ops{["linalg.fill"]} filter_result_type = tensor<1xf32> in %variant_op
  %foreach_thread_block_combiner_op, %block_combiner_op =
    transform.structured.tile_to_foreach_thread_op %grid_combiner_op tile_sizes [1] 
    ( mapping = [#gpu.thread<z>] )
  transform.structured.fuse_into_containing_op %fill_1d into %foreach_thread_block_combiner_op

  %fill_2d = transform.structured.match ops{["linalg.fill"]} filter_result_type = tensor<1x2xf32> in %variant_op
  %grid_more_parallel_op = transform.structured.match interface{LinalgOp}
    attributes{iterator_types = ["parallel", "parallel", "reduction"]} in %variant_op
  %foreach_thread_block_more_parallel_op, %block_more_parallel_op =
    transform.structured.tile_to_foreach_thread_op %grid_more_parallel_op tile_sizes [1, 1] 
    ( mapping = [#gpu.thread<z>, #gpu.thread<y>] )
  transform.structured.fuse_into_containing_op %fill_2d into %foreach_thread_block_more_parallel_op

  // Step 4. Rank-reduce and vectorize.
  // ===========================================================================
  %func = transform.structured.match ops{["func.func"]} in %variant_op
  %func_2 = transform.iree.apply_patterns %func { rank_reducing }
  %func_3 = transform.structured.vectorize %func_2

  // Step 5. Bufferize.
  // ===========================================================================
  %variant_op_2 = transform.iree.bufferize { target_gpu } %variant_op

  // Step 6. Post-bufferization mapping to blocks and threads.
  // ===========================================================================
  %func_4 = transform.structured.match ops{["func.func"]} in %variant_op_2
  %func_5 = transform.iree.foreach_thread_to_workgroup %func_4
  %func_6 = transform.iree.map_nested_foreach_thread_to_gpu_threads %func_5
      { workgroup_size = [32, 2, 1] }

  // Step 7. Post-bufferization vector distribution with rank-reduction.
  // ===========================================================================
  %func_7 = transform.iree.apply_patterns %func_6 { rank_reducing }
  %if_op = transform.structured.match ops{["scf.if"]} in %variant_op_2
  %warp = transform.iree.vector.to_warp_execute_on_lane_0 %if_op { warp_size = 32 }
  transform.iree.vector.warp_distribute %func_7
}
