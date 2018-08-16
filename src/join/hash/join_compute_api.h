/*
 * Copyright (c) 2018, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuda_runtime.h>
#include <future>

#include "join_kernels.cuh"
#include "../../gdf_table.cuh"

// TODO for Arrow integration:
//   1) replace mgpu::context_t with a new CudaComputeContext class (see the design doc)
//   2) replace cudaError_t with arrow::Status
//   3) replace input iterators & input counts with arrow::Datum
//   3) replace output iterators & output counts with arrow::ArrayData

#include <moderngpu/context.hxx>

#include <moderngpu/kernel_scan.hxx>

constexpr int DEFAULT_HASH_TABLE_OCCUPANCY = 50;
constexpr int DEFAULT_CUDA_BLOCK_SIZE = 128;
constexpr int DEFAULT_CUDA_CACHE_SIZE = 128;

template<typename size_type>
struct join_pair 
{ 
  size_type first; 
  size_type second; 
};

/// \brief Transforms the data from an array of structurs to two column.
///
/// \param[out] out An array with the indices of the common values. Stored in a 1D array with the indices of A appearing before those of B.
/// \param[in] Number of common values found)                                                                                      
/// \param[in] Common indices stored an in array of structure.
///
/// \param[in] compute_ctx The CudaComputeContext to shedule this to.
/// \param[in] Flag signifying if the order of the indices for A and B need to be swapped. This flag is used when the order of A and B are swapped to build the hash table for the smalle column.
template<typename size_type, typename join_output_pair>
void pairs_to_decoupled(mgpu::mem_t<size_type> &output, const size_type output_npairs, join_output_pair *joined, mgpu::context_t &context, bool flip_indices)
{
  if (output_npairs > 0) {
    size_type* output_data = output.data();
    auto k = [=] MGPU_DEVICE(size_type index) {
      output_data[index] = flip_indices ? joined[index].second : joined[index].first;
      output_data[index + output_npairs] = flip_indices ? joined[index].first : joined[index].second;
    };
    mgpu::transform(k, output_npairs, context);
  }
}


/* --------------------------------------------------------------------------*/
/** 
* @Synopsis  Performs a hash-based join between two sets of gdf_tables.
* 
* @Param compute_ctx The Modern GPU context
* @Param joined_output The output of the join operation
* @Param left_table The left table to join
* @Param right_table The right table to join
* @Param flip_results Flag that indicates whether the left and right tables have been 
* switched, indicating that the output indices should also be flipped
* @tparam join_type The type of join to be performed
* @tparam key_type The data type to be used for the Keys in the hash table
* @tparam index_type The data type to be used for the output indices
* 
* @Returns  cudaSuccess upon successful completion of the join. Otherwise returns
* the appropriate CUDA error code
*/
/* ----------------------------------------------------------------------------*/
template<JoinType join_type, 
         typename key_type, 
         typename index_type,
         typename size_type>
cudaError_t compute_hash_join(mgpu::context_t & compute_ctx, 
                              mgpu::mem_t<index_type> & joined_output, 
                              gdf_table<size_type> const & left_table,
                              gdf_table<size_type> const & right_table,
                              bool flip_results = false)
{

  cudaError_t error(cudaSuccess);

  // Data type used for join output results. Stored as a pair of indices 
  // (left index, right index) where left_table[left index] == right_table[right index]
  using join_output_pair = join_pair<index_type>;
  
  // The LEGACY allocator allocates the hash table array with normal cudaMalloc,
  // the non-legacy allocator uses managed memory
#ifdef HT_LEGACY_ALLOCATOR
  using multimap_type = concurrent_unordered_multimap<key_type, 
                                                      index_type, 
                                                      size_type,
                                                      std::numeric_limits<key_type>::max(), 
                                                      std::numeric_limits<index_type>::max(), 
                                                      default_hash<key_type>,
                                                      equal_to<key_type>,
                                                      legacy_allocator< thrust::pair<key_type, index_type> > >;
#else
  using multimap_type = concurrent_unordered_multimap<key_type, 
                                                      index_type, 
                                                      size_type,
                                                      std::numeric_limits<key_type>::max(), 
                                                      std::numeric_limits<size_type>::max()>;
#endif

  // Hash table will be built on the right table
  gdf_table<size_type> const & build_table{right_table};
  const size_type build_column_length{build_table.get_column_length()};
  const key_type * const build_column{static_cast<key_type*>(build_table.get_build_column_data())};

  // Allocate the hash table
  const size_type hash_table_size = (static_cast<size_type>(build_column_length) * 100 / DEFAULT_HASH_TABLE_OCCUPANCY);
  std::unique_ptr<multimap_type> hash_table(new multimap_type(hash_table_size));
  
  // FIXME: use GPU device id from the context? 
  // but moderngpu only provides cudaDeviceProp 
  // (although should be possible once we move to Arrow)
  hash_table->prefetch(0); 
  
  CUDA_RT_CALL( cudaDeviceSynchronize() );

  // build the hash table
  constexpr int block_size = DEFAULT_CUDA_BLOCK_SIZE;
  const size_type build_grid_size{(build_column_length + block_size - 1)/block_size};
  build_hash_table<<<build_grid_size, block_size>>>(hash_table.get(), 
                                                    build_column, 
                                                    build_column_length);
  
  CUDA_RT_CALL( cudaGetLastError() );

  // Allocate storage for the counter used to get the size of the join output
  size_type * d_join_output_size;
  cudaMalloc(&d_join_output_size, sizeof(size_type));
  cudaMemset(d_join_output_size, 0, sizeof(size_type));

  // Probe with the left table
  gdf_table<size_type> const & probe_table{left_table};
  const size_type probe_column_length{probe_table.get_column_length()};
  const key_type * const probe_column{static_cast<key_type*>(probe_table.get_probe_column_data())};
  const size_type probe_grid_size{(probe_column_length + block_size -1)/block_size};

  // Probe the hash table without actually building the output to simply
  // find what the size of the output will be.
  compute_join_output_size<join_type, 
                           multimap_type, 
                           key_type, 
                           size_type,
                           block_size, 
                           DEFAULT_CUDA_CACHE_SIZE>
	<<<probe_grid_size, block_size>>>(hash_table.get(), 
                                    build_table, 
                                    probe_table, 
                                    probe_column,
                                    probe_table.get_column_length(),
                                    d_join_output_size);

  CUDA_RT_CALL( cudaGetLastError() );

  // Copy output size from the device to host
  size_type h_join_output_size{0};
  CUDA_RT_CALL( cudaMemcpy(&h_join_output_size, d_join_output_size, sizeof(size_type), cudaMemcpyDeviceToHost));
  
  // If the output size is zero, return immediately
  if(0 == h_join_output_size){
    return error;
  }

  // Allocate modern GPU storage for the join output
  int dev_ordinal{0};
  join_output_pair* tempOut{nullptr};
  CUDA_RT_CALL( cudaGetDevice(&dev_ordinal));
  joined_output = mgpu::mem_t<size_type> (2 * (h_join_output_size), compute_ctx);

  // Allocate temporary device buffer for join output
  CUDA_RT_CALL( cudaMallocManaged   ( &tempOut, sizeof(join_output_pair)*h_join_output_size));
  CUDA_RT_CALL( cudaMemPrefetchAsync( tempOut , sizeof(join_output_pair)*h_join_output_size, dev_ordinal));

  // Allocate device global counter used by threads to determine output write location
  size_type *d_global_write_index{nullptr};
  CUDA_RT_CALL( cudaMalloc(&d_global_write_index, sizeof(size_type)) );
  CUDA_RT_CALL( cudaMemsetAsync(d_global_write_index, 0, sizeof(size_type), 0) );

  // Do the probe of the hash table with the probe table and generate the output for the join
  probe_hash_table<join_type, 
                   multimap_type, 
                   key_type, 
                   size_type, 
                   join_output_pair, 
                   block_size, 
                   DEFAULT_CUDA_CACHE_SIZE>
	<<<probe_grid_size, block_size>>> (hash_table.get(), 
                                     build_table, 
                                     probe_table, 
                                     probe_column, 
                                     probe_table.get_column_length(), 
                                     static_cast<join_output_pair*>(tempOut), 
                                     d_global_write_index, 
                                     h_join_output_size);

  CUDA_RT_CALL(cudaDeviceSynchronize());

  // free memory used for the counters
  CUDA_RT_CALL( cudaFree(d_global_write_index) );
  CUDA_RT_CALL( cudaFree(d_join_output_size) ); 

  // Transform the join output from an array of pairs, to an array of indices where the first
  // n/2 elements are the left indices and the last n/2 elements are the right indices
  pairs_to_decoupled(joined_output, h_join_output_size, tempOut, compute_ctx, flip_results);

  // Free temporary device buffer
  CUDA_RT_CALL( cudaFree(tempOut) );

  return error;
}

