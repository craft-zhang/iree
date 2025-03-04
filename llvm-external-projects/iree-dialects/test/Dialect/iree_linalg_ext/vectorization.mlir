// RUN: iree-dialects-opt --iree-linalg-ext-vectorization --split-input-file %s | FileCheck %s

func.func @simple_KCRS_to_KCRSsr(%arg0: tensor<1x1x32x8xf32>, %arg1: tensor<1x1x1x1x8x32xf32>) -> tensor<1x1x1x1x8x32xf32> {
  %0 = iree_linalg_ext.pack %arg0 inner_dims_pos = [3, 2] inner_tiles = [8, 32] into %arg1 : (tensor<1x1x32x8xf32> tensor<1x1x1x1x8x32xf32>) -> tensor<1x1x1x1x8x32xf32>
  return %0 : tensor<1x1x1x1x8x32xf32>
}
// CHECK-LABEL: func.func @simple_KCRS_to_KCRSsr
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[EMPTY:.+]] = tensor.empty
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]][%[[C0]], %[[C0]], %[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true, true, true]
// CHECK-SAME:    : tensor<1x1x32x8xf32>, vector<1x1x32x8xf32>
// CHECK:         %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<1x1x32x8xf32> to vector<1x1x1x1x32x8xf32>
// CHECK:         %[[TRANS:.+]] =  vector.transpose %[[BCAST]], [2, 3, 0, 1, 5, 4] : vector<1x1x1x1x32x8xf32> to vector<1x1x1x1x8x32xf32>
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[TRANS]],
// CHECK-SAME:      %[[EMPTY]][%[[C0]], %[[C0]], %[[C0]], %[[C0]], %[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true, true, true, true, true]} : vector<1x1x1x1x8x32xf32>, tensor<1x1x1x1x8x32xf32>

// -----

func.func @simple_pad_and_pack(%input: tensor<5x1xf32>, %output: tensor<1x1x8x2xf32>, %pad: f32) -> tensor<1x1x8x2xf32> {
  %0 = iree_linalg_ext.pack %input padding_value(%pad : f32) inner_dims_pos = [0, 1] inner_tiles = [8, 2] into %output : (tensor<5x1xf32> tensor<1x1x8x2xf32>) -> tensor<1x1x8x2xf32>
  return %0 : tensor<1x1x8x2xf32>
}
// CHECK-LABEL: func.func @simple_pad_and_pack
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[PAD:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[EMPTY:.+]] = tensor.empty
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]][%[[C0]], %[[C0]]], %[[PAD]]
// CHECK-SAME:      : tensor<5x1xf32>, vector<8x2xf32>
// CHECK:         %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<8x2xf32> to vector<1x1x8x2xf32>
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[BCAST]],
// CHECK-SAME:      %[[EMPTY]][%[[C0]], %[[C0]], %[[C0]], %[[C0]]]
// CHECK-SAME:     {in_bounds = [true, true, true, true]} : vector<1x1x8x2xf32>, tensor<1x1x8x2xf32>

// -----

func.func @simple_NC_to_CNnc(%arg0: tensor<32x8xf32>, %arg1: tensor<1x1x32x8xf32>) -> tensor<1x1x32x8xf32>{
  %0 = iree_linalg_ext.pack %arg0 outer_dims_perm = [1, 0] inner_dims_pos = [0, 1] inner_tiles = [32, 8] into %arg1 : (tensor<32x8xf32> tensor<1x1x32x8xf32>) -> tensor<1x1x32x8xf32>
  return %0 : tensor<1x1x32x8xf32>
}
// CHECK-LABEL: func.func @simple_NC_to_CNnc
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[EMPTY:.+]] = tensor.empty
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]][%[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true]}
// CHECK-SAME:    : tensor<32x8xf32>, vector<32x8xf32>
// CHECK:         %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<32x8xf32> to vector<1x1x32x8xf32>
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[BCAST]],
// CHECK-SAME:      %[[EMPTY]][%[[C0]], %[[C0]], %[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true, true, true]} : vector<1x1x32x8xf32>, tensor<1x1x32x8xf32>

// -----

func.func @KCRS_to_KCRSsr(%arg0: tensor<1x1x128x64xf32>, %arg1: tensor<1x1x4x8x8x32xf32>) -> tensor<1x1x4x8x8x32xf32> {
  %0 = iree_linalg_ext.pack %arg0 inner_dims_pos = [3, 2] inner_tiles = [8, 32] into %arg1 : (tensor<1x1x128x64xf32> tensor<1x1x4x8x8x32xf32>) -> tensor<1x1x4x8x8x32xf32>
  return %0 : tensor<1x1x4x8x8x32xf32>
}
// CHECK-DAG:   #[[MAP0:.+]] = affine_map<(d0) -> (d0 * 32)>
// CHECK-DAG:   #[[MAP1:.+]] = affine_map<(d0) -> (d0 * 8)>
// CHECK-LABEL: func.func @KCRS_to_KCRSsr
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[CST:.+]] = arith.constant 0.000000e+00 : f32
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:     %[[C4:.+]] = arith.constant 4 : index
// CHECK-DAG:     %[[C8:.+]] = arith.constant 8 : index
// CHECK:         %[[RES0:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C4]] step %[[C1]]
// CHECK-SAME:      iter_args(%[[ITER0:.+]] = %[[OUT]])
// CHECK:           %[[RES1:.+]] = scf.for %[[J:.+]] = %[[C0]] to %[[C8]] step %[[C1]]
// CHECK-SAME:        iter_args(%[[ITER1:.+]] = %[[ITER0]])
// CHECK-DAG:         %[[IDX2:.+]] = affine.apply #[[MAP0]](%[[I]])
// CHECK-DAG:         %[[IDX3:.+]] = affine.apply #[[MAP1]](%[[J]])
// CHECK:             %[[READ:.+]] = vector.transfer_read %[[IN]]
// CHECK-SAME:          [%[[C0]], %[[C0]], %[[IDX2]], %[[IDX3]]]
// CHECK-SAME:          {in_bounds = [true, true, true, true]}
// CHECK-SAME:          : tensor<1x1x128x64xf32>, vector<1x1x32x8xf32>
// CHECK:             %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<1x1x32x8xf32> to vector<1x1x1x1x32x8xf32>
// CHECK:             %[[TRANS:.+]] = vector.transpose %[[BCAST]], [2, 3, 0, 1, 5, 4] : vector<1x1x1x1x32x8xf32> to vector<1x1x1x1x8x32xf32>
// CHECK:             %[[WRITE:.+]] = vector.transfer_write %[[TRANS]]
// CHECK-SAME:          %[[ITER1]][%[[C0]], %[[C0]], %[[I]], %[[J]], %[[C0]], %[[C0]]
// CHECK-SAME:          {in_bounds = [true, true, true, true, true, true]}
// CHECK-SAME:          : vector<1x1x1x1x8x32xf32>, tensor<1x1x4x8x8x32xf32>
// CHECK:             scf.yield %[[WRITE]]
// CHECK:           }
// CHECK:           scf.yield %[[RES1]]
// CHECK:         }
// CHECK:         return %[[RES0]]

// -----

func.func @pad_and_pack(%arg0: tensor<13x15xf32>, %arg1: tensor<2x8x8x2xf32>, %arg2: f32) -> tensor<2x8x8x2xf32> {
  %0 = iree_linalg_ext.pack %arg0 padding_value(%arg2 : f32) inner_dims_pos = [0, 1] inner_tiles = [8, 2] into %arg1 : (tensor<13x15xf32> tensor<2x8x8x2xf32>) -> tensor<2x8x8x2xf32>
  return %0 : tensor<2x8x8x2xf32>
}
// CHECK-DAG:   #[[MAP0:.+]] = affine_map<(d0) -> (d0 * 8)>
// CHECK-DAG:   #[[MAP1:.+]] = affine_map<(d0) -> (d0 * -8 + 13, 8)>
// CHECK-DAG:   #[[MAP2:.+]] = affine_map<(d0) -> (d0 * 2)>
// CHECK-DAG:   #[[MAP3:.+]] = affine_map<(d0) -> (d0 * -2 + 15, 2)>
// CHECK-LABEL: func.func @pad_and_pack
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[PAD:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:     %[[C2:.+]] = arith.constant 2 : index
// CHECK-DAG:     %[[C8:.+]] = arith.constant 8 : index
// CHECK:         %[[RES0:.+]] = scf.for %[[I:.+]] = %[[C0]] to %[[C2]] step %[[C1]]
// CHECK-SAME:      iter_args(%[[ITER0:.+]] = %[[OUT]])
// CHECK:           %[[RES1:.+]] = scf.for %[[J:.+]] = %[[C0]] to %[[C8]] step %[[C1]]
// CHECK-SAME:        iter_args(%[[ITER1:.+]] = %[[ITER0]])
// CHECK-DAG:         %[[IDX0:.+]] = affine.apply #[[MAP0]](%[[I]])
// CHECK-DAG:         %[[SZ0:.+]] = affine.min #[[MAP1]](%[[I]])
// CHECK-DAG:         %[[IDX1:.+]] = affine.apply #[[MAP2]](%[[J]])
// CHECK-DAG:         %[[SZ1:.+]] = affine.min #[[MAP3]](%[[J]])
// CHECK:             %[[SLICE:.+]] = tensor.extract_slice %[[IN]][%[[IDX0]], %[[IDX1]]] [%[[SZ0]], %[[SZ1]]]
// CHECK:             %[[READ:.+]] = vector.transfer_read %[[SLICE]]
// CHECK-SAME:          [%[[C0]], %[[C0]]], %[[PAD]]
// CHECK-SAME:          : tensor<?x?xf32>, vector<8x2xf32>
// CHECK:             %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<8x2xf32> to vector<1x1x8x2xf32>
// CHECK:             %[[WRITE:.+]] = vector.transfer_write %[[BCAST]]
// CHECK-SAME:          %[[ITER1]][%[[I]], %[[J]], %[[C0]], %[[C0]]
// CHECK-SAME:          {in_bounds = [true, true, true, true]}
// CHECK-SAME:          : vector<1x1x8x2xf32>, tensor<2x8x8x2xf32>
// CHECK:             scf.yield %[[WRITE]]
// CHECK:           }
// CHECK:           scf.yield %[[RES1]]
// CHECK:         }
// CHECK:         return %[[RES0]]

// -----

func.func @KC_to_CKck(%arg0: tensor<128x256xf32>, %arg1: tensor<32x4x32x8xf32>) -> tensor<32x4x32x8xf32> {
  %0 = iree_linalg_ext.pack %arg0 outer_dims_perm = [1, 0] inner_dims_pos = [0, 1] inner_tiles = [32, 8] into %arg1 : (tensor<128x256xf32> tensor<32x4x32x8xf32>) -> tensor<32x4x32x8xf32>
  return %0 : tensor<32x4x32x8xf32>
}
// CHECK-DAG:   #[[MAP0:.+]] = affine_map<(d0) -> (d0 * 32)>
// CHECK-DAG:   #[[MAP1:.+]] = affine_map<(d0) -> (d0 * 8)>
// CHECK-LABEL: func.func @KC_to_CKck
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[CST:.+]] = arith.constant 0.000000e+00 : f32
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[C1:.+]] = arith.constant 1 : index
// CHECK-DAG:     %[[C4:.+]] = arith.constant 4 : index
// CHECK-DAG:     %[[C32:.+]] = arith.constant 32 : index
// CHECK:         %[[RES0:.+]] = scf.for %[[C:.+]] = %[[C0]] to %[[C32]] step %[[C1]]
// CHECK-SAME:      iter_args(%[[ITER0:.+]] = %[[OUT]])
// CHECK:           %[[RES1:.+]] = scf.for %[[K:.+]] = %[[C0]] to %[[C4]] step %[[C1]]
// CHECK-SAME:        iter_args(%[[ITER1:.+]] = %[[ITER0]])
// CHECK-DAG:         %[[IN_K:.+]] = affine.apply #[[MAP0]](%[[K]])
// CHECK-DAG:         %[[IN_C:.+]] = affine.apply #[[MAP1]](%[[C]])
// CHECK:             %[[READ:.+]] = vector.transfer_read %[[IN]]
// CHECK-SAME:          [%[[IN_K]], %[[IN_C]]]
// CHECK-SAME:          {in_bounds = [true, true]}
// CHECK-SAME:          : tensor<128x256xf32>, vector<32x8xf32>
// CHECK:             %[[BCAST:.+]] = vector.broadcast %[[READ]] : vector<32x8xf32> to vector<1x1x32x8xf32>
// CHECK:             %[[WRITE:.+]] = vector.transfer_write %[[BCAST]]
// CHECK-SAME:          %[[ITER1]][%[[C]], %[[K]], %[[C0]], %[[C0]]
// CHECK-SAME:          {in_bounds = [true, true, true, true]}
// CHECK-SAME:          : vector<1x1x32x8xf32>, tensor<32x4x32x8xf32>
// CHECK:             scf.yield %[[WRITE]]
// CHECK:           }
// CHECK:           scf.yield %[[RES1]]
// CHECK:         }
// CHECK:         return %[[RES0]]

// -----

func.func @simple_KCRSsr_to_KCRS(%arg0: tensor<1x1x1x1x8x32xf32>, %arg1: tensor<1x1x32x8xf32>) -> tensor<1x1x32x8xf32> {
  %0 = iree_linalg_ext.unpack %arg0 inner_dims_pos = [3, 2] inner_tiles = [8, 32] into %arg1 : (tensor<1x1x1x1x8x32xf32> tensor<1x1x32x8xf32>) -> tensor<1x1x32x8xf32>
  return %0 : tensor<1x1x32x8xf32>
}
// CHECK-LABEL: func.func @simple_KCRSsr_to_KCRS
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[ZERO:.+]] = arith.constant 0.000000e+00 : f32
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]]
// CHECK-SAME:      [%[[C0]], %[[C0]], %[[C0]], %[[C0]], %[[C0]], %[[C0]]], %[[ZERO]]
// CHECK-SAME:      {in_bounds = [true, true]} : tensor<1x1x1x1x8x32xf32>, vector<8x32xf32>
// CHECK:         %[[TRANSP:.+]] = vector.transpose %[[READ]], [1, 0]
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[TRANSP]]
// CHECK-SAME:      %[[OUT]][%[[C0]], %[[C0]], %[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true]} : vector<32x8xf32>, tensor<1x1x32x8xf32>
// CHECK:         return %[[WRITE]]

// -----

func.func @simple_unpack_and_extract_slice(%input: tensor<1x1x8x2xf32>, %output: tensor<5x1xf32>) -> tensor<5x1xf32> {
  %0 = iree_linalg_ext.unpack %input inner_dims_pos = [0, 1] inner_tiles = [8, 2] into %output : (tensor<1x1x8x2xf32> tensor<5x1xf32>) -> tensor<5x1xf32>
  return %0 : tensor<5x1xf32>
}
// CHECK-LABEL: func.func @simple_unpack_and_extract_slice
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[C0:.+]] = arith.constant 0 : index
// CHECK-DAG:     %[[ZERO:.+]] = arith.constant 0.000000e+00 : f32
// CHECK-DAG:     %[[EMPTY:.+]] = tensor.empty() : tensor<8x2xf32>
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]]
// CHECK-SAME:      [%[[C0]], %[[C0]], %[[C0]], %[[C0]]], %[[ZERO]]
// CHECK-SAME:      {in_bounds = [true, true]} : tensor<1x1x8x2xf32>, vector<8x2xf32>
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[READ]],
// CHECK-SAME:      %[[EMPTY]][%[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true]} : vector<8x2xf32>, tensor<8x2xf32>
// CHECK:         %[[RES:.+]] = tensor.extract_slice %[[WRITE]]
// CHECK-SAME:      [0, 0] [5, 1] [1, 1] : tensor<8x2xf32> to tensor<5x1xf32>
// CHECK:         return %[[RES:.+]]

// -----

func.func @simple_CNnc_to_NC(%arg0: tensor<1x1x32x8xf32>, %arg1: tensor<32x8xf32>) -> tensor<32x8xf32>{
  %0 = iree_linalg_ext.unpack %arg0 outer_dims_perm = [1, 0] inner_dims_pos = [0, 1] inner_tiles = [32, 8] into %arg1 : (tensor<1x1x32x8xf32> tensor<32x8xf32>) -> tensor<32x8xf32>
  return %0 : tensor<32x8xf32>
}
// CHECK-LABEL: func.func @simple_CNnc_to_NC
// CHECK-SAME:    %[[IN:[A-Za-z0-9]+]]:
// CHECK-SAME:    %[[OUT:[A-Za-z0-9]+]]:
// CHECK-DAG:     %[[ZERO:.+]] = arith.constant 0.000000e+00 : f32
// CHECK-DAG:     %[[EMPTY:.+]] = tensor.empty() : tensor<32x8xf32>
// CHECK:         %[[READ:.+]] = vector.transfer_read %[[IN]]
// CHECK-SAME:      [%[[C0]], %[[C0]], %[[C0]], %[[C0]]], %[[ZERO]]
// CHECK-SAME:      {in_bounds = [true, true]} : tensor<1x1x32x8xf32>, vector<32x8xf32>
// CHECK:         %[[WRITE:.+]] = vector.transfer_write %[[READ]],
// CHECK-SAME:      %[[EMPTY]][%[[C0]], %[[C0]]]
// CHECK-SAME:      {in_bounds = [true, true]} : vector<32x8xf32>, tensor<32x8xf32>
// CHECK:         return %[[WRITE]]
