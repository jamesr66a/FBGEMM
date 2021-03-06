/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <cpuinfo.h>
#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <numeric>

#include "OptimizedKernelsAvx2.h"
#include "fbgemm/Fbgemm.h"

namespace fbgemm {

template <typename T, typename accT, int SPATIAL_DIM>
PackAWithIm2Col<T, accT, SPATIAL_DIM>::PackAWithIm2Col(
    const conv_param_t<SPATIAL_DIM>& conv_p,
    const T* sdata,
    inpType* pmat,
    int32_t a_zero_pt,
    int32_t* row_offset,
    bool b_symmetric,
    const BlockingFactors* params)
    : PackMatrix<PackAWithIm2Col<T, accT, SPATIAL_DIM>, T, accT>(
          conv_p.MB *
              std::accumulate(
                  conv_p.OUT_DIM.begin(),
                  conv_p.OUT_DIM.end(),
                  1,
                  std::multiplies<int>()),
          std::accumulate(
              conv_p.K.begin(),
              conv_p.K.end(),
              1,
              std::multiplies<int>()) *
              conv_p.IC,
          pmat,
          conv_p.G,
          params),
      conv_p_(conv_p),
      sdata_(sdata),
      a_zero_pt_(a_zero_pt) {
  static_assert(
      SPATIAL_DIM == 2 || SPATIAL_DIM == 3, "unsupported conv dimension ");
  if (!cpuinfo_initialize()) {
    throw std::runtime_error("Failed to initialize cpuinfo!");
  }
  if ((!fbgemmHasAvx512VnniSupport() && !fbgemmHasAvx512Support() &&
       !fbgemmHasAvx2Support())) {
    assert(0 && "unknown architecure");
  }

  if (params) {
    BaseType::brow_ = params->MCB;
    BaseType::bcol_ = params->KCB;
    row_interleave_B_ = params->ROW_INTERLEAVE;
  } else {
    if (fbgemmHasAvx512VnniSupport()) {
      BaseType::brow_ = PackingTraits<T, accT, inst_set_t::avx512_vnni>::MCB;
      BaseType::bcol_ = PackingTraits<T, accT, inst_set_t::avx512_vnni>::KCB;
      row_interleave_B_ =
          PackingTraits<T, accT, inst_set_t::avx512_vnni>::ROW_INTERLEAVE;
    } else if (fbgemmHasAvx512Support()) {
      BaseType::brow_ = PackingTraits<T, accT, inst_set_t::avx512>::MCB;
      BaseType::bcol_ = PackingTraits<T, accT, inst_set_t::avx512>::KCB;
      row_interleave_B_ =
          PackingTraits<T, accT, inst_set_t::avx512>::ROW_INTERLEAVE;
    } else {
      // AVX2
      BaseType::brow_ = PackingTraits<T, accT, inst_set_t::avx2>::MCB;
      BaseType::bcol_ = PackingTraits<T, accT, inst_set_t::avx2>::KCB;
      row_interleave_B_ =
          PackingTraits<T, accT, inst_set_t::avx2>::ROW_INTERLEAVE;
    }
  }

  if (BaseType::numCols() % conv_p.G != 0) {
    throw std::runtime_error(
        "groups = " + std::to_string(conv_p.G) +
        " does not divide numCols = " + std::to_string(BaseType::numCols()));
  }
  if (pmat) {
    BaseType::buf_ = pmat;
  } else {
    BaseType::bufAllocatedHere_ = true;
    BaseType::buf_ = static_cast<T*>(
        fbgemmAlignedAlloc(64, BaseType::brow_ * BaseType::bcol_ * sizeof(T)));
    // aligned_alloc(64, BaseType::brow_ * BaseType::bcol_ * sizeof(T)));
  }
  if (!b_symmetric) {
    if (row_offset) {
      rowOffsetAllocatedHere = false;
      row_offset_ = row_offset;
    } else {
      rowOffsetAllocatedHere = true;
      row_offset_ = static_cast<int32_t*>(
          fbgemmAlignedAlloc(64, BaseType::brow_ * sizeof(int32_t)));
    }
  }
}

template <
    int SPATIAL_DIM,
    int IC,
    int IN_DIM_H,
    int IN_DIM_W,
    int K_H,
    int K_W,
    int STRIDE_H,
    int STRIDE_W,
    int PAD_W,
    int PAD_H,
    int OUT_DIM_H,
    int OUT_DIM_W,
    int BCOL,
    int COL_SIZE,
    int COL_P_SIZE>
void pack_a_with_im2col_opt(
    const block_type_t& block,
    const uint8_t* sdata,
    uint8_t* out,
    int32_t a_zero_pt,
    int32_t* row_offset_buf,
    bool row_offset_acc) {
  for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
    int n = i / (OUT_DIM_H * OUT_DIM_W);
    int hw = i % (OUT_DIM_H * OUT_DIM_W);
    int w = hw % OUT_DIM_W;
    int h = hw / OUT_DIM_W;

    // j refers to column index within block
    int j = 0;
    // r and s iterate over K_H and K_W, respectively
    for (int r = 0; r < K_H; ++r) {
      int h_in = -PAD_H + h * STRIDE_H + r;
      if (h_in < 0 || h_in >= IN_DIM_H) {
        // Short-circuit if h_in is in padding.
        std::memset(
            out + (i - block.row_start) * BCOL + j,
            a_zero_pt,
            sizeof(uint8_t) * K_W * IC);
        j += K_W * IC;
        continue;
      }

      int s = 0;
      // left_pad_len : the number of spatial pixels we need to pad at the
      // beginning
      int left_pad_len = PAD_W - w * STRIDE_W;
      if (left_pad_len > 0) {
        std::memset(
            out + (i - block.row_start) * BCOL + j,
            a_zero_pt,
            sizeof(uint8_t) * left_pad_len * IC);
        s += left_pad_len;
      }

      // mid_len : the number of spatial pixels that we handle normally
      // (no padding)
      int mid_len = std::min(IN_DIM_W + PAD_W - w * STRIDE_W, K_W) - s;
      std::memcpy(
          out + (i - block.row_start) * BCOL + j + s * IC,
          sdata +
              ((n * IN_DIM_H + h_in) * IN_DIM_W + -PAD_W + w * STRIDE_W + s) *
                  IC,
          sizeof(uint8_t) * mid_len * IC);
      s += mid_len;

      // right_pad_len : the number of spatial pixels we need to pad at the end
      int right_pad_len = K_W - s;
      if (right_pad_len > 0) {
        std::memset(
            out + (i - block.row_start) * BCOL + j + s * IC,
            a_zero_pt,
            sizeof(uint8_t) * right_pad_len * IC);
      }
      j += K_W * IC;
    } // r loop

    // zero fill
    // Please see the comment in PackAMatrix.cc for zero vs zero_pt fill.
    if (COL_P_SIZE - COL_SIZE > 0) {
      std::memset(
          &out[(i - block.row_start) * BCOL + COL_SIZE],
          0,
          sizeof(uint8_t) * COL_P_SIZE - COL_SIZE);
    }

    if (row_offset_buf) {
      int32_t row_sum =
          row_offset_acc ? row_offset_buf[i - block.row_start] : 0;
      row_sum += reduceAvx2(out + (i - block.row_start) * BCOL, COL_SIZE);
      row_offset_buf[i - block.row_start] = row_sum;
    }
  }
}

template <typename T, typename accT, int SPATIAL_DIM>
void PackAWithIm2Col<T, accT, SPATIAL_DIM>::pack(const block_type_t& block) {
  block_type_t block_p = {block.row_start,
                          block.row_size,
                          block.col_start,
                          (block.col_size + row_interleave_B_ - 1) /
                              row_interleave_B_ * row_interleave_B_};
  BaseType::packedBlock(block_p);
  T* out = BaseType::getBuf();
  // accumulate into row offset?
  bool row_offset_acc =
      (block.col_start % (this->numCols() / this->numGroups())) != 0;
  int32_t* row_offset_buf = getRowOffsetBuffer();

  bool point_wise = true;
  for (int d = 0; d < SPATIAL_DIM; ++d) {
    if (conv_p_.K[d] != 1 || conv_p_.pad[d] != 0 || conv_p_.stride[d] != 1 ||
        conv_p_.dilation[d] != 1) {
      point_wise = false;
      break;
    }
  }
  for (int d = SPATIAL_DIM; d < SPATIAL_DIM * 2; ++d) {
    if (conv_p_.pad[d] != 0) {
      point_wise = false;
      break;
    }
  }

  // reduceAvx2 only written for T == uint8_t
  static_assert(
      std::is_same<T, uint8_t>::value,
      "PackAWithIm2Col<T, accT>::pack only works for T == uint8_t");
  if (point_wise) {
    int32_t ld = this->numCols();
    if (row_offset_buf) {
      for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
        int buf_idx = i - block.row_start;
        memcpy(
            out + buf_idx * BaseType::blockColSize(),
            sdata_ + i * ld + block.col_start,
            block.col_size * sizeof(T));
        // zero fill
        for (int j = block.col_size; j < block_p.col_size; ++j) {
          out[buf_idx * BaseType::blockColSize() + j] = 0;
        }
        int32_t row_sum =
            row_offset_acc ? row_offset_buf[i - block.row_start] : 0;
        row_sum +=
            reduceAvx2(sdata_ + i * ld + block.col_start, block.col_size);
        row_offset_buf[i - block.row_start] = row_sum;
      }
    } else {
      for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
        int buf_idx = i - block.row_start;
        memcpy(
            out + buf_idx * BaseType::blockColSize(),
            sdata_ + i * ld + block.col_start,
            block.col_size * sizeof(T));
        // zero fill
        for (int j = block.col_size; j < block_p.col_size; ++j) {
          out[buf_idx * BaseType::blockColSize() + j] = 0;
        }
      }
    }

    return;
  }

  int ic_per_group = conv_p_.IC / conv_p_.G;

  if (SPATIAL_DIM == 2 && conv_p_.IC == 3 && conv_p_.IN_DIM[0] == 224 &&
      conv_p_.IN_DIM[1] == 224 && conv_p_.G == 1 && conv_p_.K[0] == 7 &&
      conv_p_.K[1] == 7 && conv_p_.stride[0] == 2 && conv_p_.stride[1] == 2 &&
      conv_p_.pad[0] == 3 && conv_p_.pad[1] == 3 && block.col_size == 147 &&
      block_p.col_size == 148 && block.col_start == 0 &&
      std::is_same<T, uint8_t>::value) {
    if (BaseType::blockColSize() == 256) {
      pack_a_with_im2col_opt<
          SPATIAL_DIM,
          3,
          224,
          224,
          7,
          7,
          2,
          2,
          3,
          3,
          112,
          112,
          256,
          147,
          148>(
          block,
          reinterpret_cast<const uint8_t*>(sdata_),
          reinterpret_cast<uint8_t*>(out),
          a_zero_pt_,
          row_offset_buf,
          row_offset_acc);
      return;
    } else if (BaseType::blockColSize() == 512) {
      pack_a_with_im2col_opt<
          SPATIAL_DIM,
          3,
          224,
          224,
          7,
          7,
          2,
          2,
          3,
          3,
          112,
          112,
          512,
          147,
          148>(
          block,
          reinterpret_cast<const uint8_t*>(sdata_),
          reinterpret_cast<uint8_t*>(out),
          a_zero_pt_,
          row_offset_buf,
          row_offset_acc);
      return;
    }
  }

  for (int i = block.row_start; i < block.row_start + block.row_size; ++i) {
    if (SPATIAL_DIM == 2) { // static if
      int n = i / (conv_p_.OUT_DIM[0] * conv_p_.OUT_DIM[1]);
      int hw = i % (conv_p_.OUT_DIM[0] * conv_p_.OUT_DIM[1]);
      int w = hw % conv_p_.OUT_DIM[1];
      int h = hw / conv_p_.OUT_DIM[1];
      for (int j = block.col_start;
           j < block.col_start + block.col_size + ic_per_group - 1;
           j += ic_per_group) {
        int j_blk_id = j / ic_per_group;
        // max( j_blk_id * IC, START)  -> min( END, (j_blk_id + 1) * IC )
        int j_blk_start = std::max(j_blk_id * ic_per_group, block.col_start);
        int j_blk_end = std::min(
            (j_blk_id + 1) * ic_per_group, block.col_start + block.col_size);
        if (j_blk_start >= j_blk_end) {
          break;
        }

        int grs = j / ic_per_group;
        int s = grs % conv_p_.K[1];
        int r = grs / conv_p_.K[1] % conv_p_.K[0];
        int g = grs / conv_p_.K[1] / conv_p_.K[0];

        int h_in = -conv_p_.pad[0] + h * conv_p_.stride[0] + r;
        int w_in = -conv_p_.pad[1] + w * conv_p_.stride[1] + s;

        if (h_in < 0 || h_in >= conv_p_.IN_DIM[0] || w_in < 0 ||
            w_in >= conv_p_.IN_DIM[1]) {
          // Please note that padding for convolution should be filled with
          // zero_pt
          std::memset(
              out + (i - block.row_start) * BaseType::blockColSize() +
                  (j_blk_start - block.col_start),
              a_zero_pt_,
              sizeof(T) * (j_blk_end - j_blk_start));
        } else {
          std::memcpy(
              out + (i - block.row_start) * BaseType::blockColSize() +
                  j_blk_start - block.col_start,
              sdata_ +
                  ((n * conv_p_.IN_DIM[0] + h_in) * conv_p_.IN_DIM[1] + w_in) *
                      conv_p_.IC +
                  g * ic_per_group + (j_blk_start % ic_per_group),
              sizeof(T) * (j_blk_end - j_blk_start));
        }
      }
    } else if (SPATIAL_DIM == 3) { // static if
      int n =
          i / (conv_p_.OUT_DIM[0] * conv_p_.OUT_DIM[1] * conv_p_.OUT_DIM[2]);
      int thw =
          i % (conv_p_.OUT_DIM[0] * conv_p_.OUT_DIM[1] * conv_p_.OUT_DIM[2]);
      int w = thw % conv_p_.OUT_DIM[2];
      int h = thw / conv_p_.OUT_DIM[2] % conv_p_.OUT_DIM[1];
      int t = thw / conv_p_.OUT_DIM[2] / conv_p_.OUT_DIM[1];
      for (int j = block.col_start;
           j < block.col_start + block.col_size + ic_per_group - 1;
           j += ic_per_group) {
        int j_blk_id = j / ic_per_group;
        // max( j_blk_id * IC, START)  -> min( END, (j_blk_id + 1) * IC )
        int j_blk_start = std::max(j_blk_id * ic_per_group, block.col_start);
        int j_blk_end = std::min(
            (j_blk_id + 1) * ic_per_group, block.col_start + block.col_size);
        if (j_blk_start >= j_blk_end) {
          break;
        }

        int gqrs = j / ic_per_group;
        int s = gqrs % conv_p_.K[2];
        int r = gqrs / conv_p_.K[2] % conv_p_.K[1];
        int q = gqrs / conv_p_.K[2] / conv_p_.K[1] % conv_p_.K[0];
        int g = gqrs / conv_p_.K[2] / conv_p_.K[1] / conv_p_.K[0];

        int t_in = -conv_p_.pad[0] + t * conv_p_.stride[0] + q;
        int h_in = -conv_p_.pad[1] + h * conv_p_.stride[1] + r;
        int w_in = -conv_p_.pad[2] + w * conv_p_.stride[2] + s;

        if (t_in < 0 || t_in >= conv_p_.IN_DIM[0] || h_in < 0 ||
            h_in >= conv_p_.IN_DIM[1] || w_in < 0 ||
            w_in >= conv_p_.IN_DIM[2]) {
          // Please note that padding for convolution should be filled with
          // zero_pt
          std::memset(
              &out
                  [(i - block.row_start) * BaseType::blockColSize() +
                   (j_blk_start - block.col_start)],
              a_zero_pt_,
              sizeof(T) * (j_blk_end - j_blk_start));
        } else {
          std::memcpy(
              out + (i - block.row_start) * BaseType::blockColSize() +
                  j_blk_start - block.col_start,
              sdata_ +
                  (((n * conv_p_.IN_DIM[0] + t_in) * conv_p_.IN_DIM[1] + h_in) *
                       conv_p_.IN_DIM[2] +
                   w_in) *
                      conv_p_.IC +
                  g * ic_per_group + (j_blk_start % ic_per_group),
              sizeof(T) * (j_blk_end - j_blk_start));
        }
      }
    }

    // zero fill
    // Please see the comment in PackAMatrix.cc for zero vs zero_pt fill.
    if ((block_p.col_start + block_p.col_size) -
            (block.col_start + block.col_size) >
        0) {
      std::memset(
          &out
              [(i - block.row_start) * BaseType::blockColSize() +
               (block.col_size)],
          0,
          sizeof(T) *
              ((block_p.col_start + block_p.col_size) -
               (block.col_start + block.col_size)));
    }

    if (row_offset_buf) {
      int32_t row_sum =
          row_offset_acc ? row_offset_buf[i - block.row_start] : 0;
      row_sum += reduceAvx2(
          out + (i - block.row_start) * this->blockColSize(), block.col_size);
      row_offset_buf[i - block.row_start] = row_sum;
    }
  } // for each i
}

template <typename T, typename accT, int SPATIAL_DIM>
void PackAWithIm2Col<T, accT, SPATIAL_DIM>::printPackedMatrix(
    std::string name) {
  std::cout << name << ":"
            << "[" << BaseType::numPackedRows() << ", "
            << BaseType::numPackedCols() << "]" << std::endl;

  T* out = BaseType::getBuf();
  for (auto r = 0; r < BaseType::numPackedRows(); ++r) {
    for (auto c = 0; c < BaseType::numPackedCols(); ++c) {
      T val = out[r * BaseType::blockColSize() + c];
      if (std::is_integral<T>::value) {
        // cast to int64 because cout doesn't print int8_t type directly
        std::cout << std::setw(5) << static_cast<int64_t>(val) << " ";
      } else {
        std::cout << std::setw(5) << val << " ";
      }
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

template <typename T, typename accT, int SPATIAL_DIM>
int PackAWithIm2Col<T, accT, SPATIAL_DIM>::rowOffsetBufferSize(
    const BlockingFactors* params) {
  if (cpuinfo_initialize()) {
    if (params) {
      return params->MCB;
    } else {
      if (fbgemmHasAvx512VnniSupport()) {
        return PackingTraits<T, accT, inst_set_t::avx512_vnni>::MCB;
      } else if (fbgemmHasAvx512Support()) {
        return PackingTraits<T, accT, inst_set_t::avx512>::MCB;
      } else if (fbgemmHasAvx2Support()) {
        return PackingTraits<T, accT, inst_set_t::avx2>::MCB;
      } else {
        // TODO: Have default slower path
        assert(0 && "unsupported architecture");
        return -1;
      }
    }
  } else {
    throw std::runtime_error("Failed to initialize cpuinfo!");
  }
}

template class PackAWithIm2Col<uint8_t, int32_t>;
template class PackAWithIm2Col<uint8_t, int16_t>;
template class PackAWithIm2Col<uint8_t, int32_t, 3>;
template class PackAWithIm2Col<uint8_t, int16_t, 3>;

} // namespace fbgemm
