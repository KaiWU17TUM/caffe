#ifdef USE_CUDNN
#include <algorithm>
#include <vector>

#include "caffe/layers/cudnn_conv_layer.hpp"

namespace caffe {

// Set to three for the benefit of the backward pass, which
// can use separate streams for calculating the gradient w.r.t.
// bias, filter weights, and bottom data for each group independently
#define CUDNN_STREAMS_PER_GROUP 1 // 3

/**
 * TODO(dox) explain cuDNN interface
 */
template <typename Dtype>
void CuDNNConvolutionLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  ConvolutionLayer<Dtype>::LayerSetUp(bottom, top);
  // Initialize CUDA streams and cuDNN.
  stream_         = new cudaStream_t[this->group_ * CUDNN_STREAMS_PER_GROUP];
  handle_         = new cudnnHandle_t[this->group_ * CUDNN_STREAMS_PER_GROUP];

#if CUDNN_VERSION_MIN(8, 0, 0) && defined(USE_CUDNN_FRONTEND)
  // workspace data
  workspaceSizeInBytes = 0;
  workspaceData = NULL;
  workspace = new void*[this->group_ * CUDNN_STREAMS_PER_GROUP];
  workspace_size_ = 0;

  for (int g = 0; g < this->group_ * CUDNN_STREAMS_PER_GROUP; g++) {
    CUDA_CHECK(cudaStreamCreate(&stream_[g]));
    CUDNN_CHECK(cudnnCreate(&handle_[g]));
    CUDNN_CHECK(cudnnSetStream(handle_[g], stream_[g]));
    workspace[g] = NULL;
  }

  // Set the indexing parameters.
  bias_offset_ = (this->num_output_ / this->group_);
  
#else
  // Initialize algorithm arrays
  fwd_algo_       = new cudnnConvolutionFwdAlgo_t[bottom.size()];
  bwd_filter_algo_= new cudnnConvolutionBwdFilterAlgo_t[bottom.size()];
  bwd_data_algo_  = new cudnnConvolutionBwdDataAlgo_t[bottom.size()];

  // initialize size arrays
  workspace_fwd_sizes_ = new size_t[bottom.size()];
  workspace_bwd_filter_sizes_ = new size_t[bottom.size()];
  workspace_bwd_data_sizes_ = new size_t[bottom.size()];

  // workspace data
  workspaceSizeInBytes = 0;
  workspaceData = NULL;
  workspace = new void*[this->group_ * CUDNN_STREAMS_PER_GROUP];

  for (size_t i = 0; i < bottom.size(); ++i) {
    // initialize all to default algorithms
    fwd_algo_[i] = (cudnnConvolutionFwdAlgo_t)0;
    bwd_filter_algo_[i] = (cudnnConvolutionBwdFilterAlgo_t)0;
    bwd_data_algo_[i] = (cudnnConvolutionBwdDataAlgo_t)0;
    // default algorithms don't require workspace
    workspace_fwd_sizes_[i] = 0;
    workspace_bwd_data_sizes_[i] = 0;
    workspace_bwd_filter_sizes_[i] = 0;
  }

  for (int g = 0; g < this->group_ * CUDNN_STREAMS_PER_GROUP; g++) {
    CUDA_CHECK(cudaStreamCreate(&stream_[g]));
    CUDNN_CHECK(cudnnCreate(&handle_[g]));
    CUDNN_CHECK(cudnnSetStream(handle_[g], stream_[g]));
    workspace[g] = NULL;
  }

  // Set the indexing parameters.
  bias_offset_ = (this->num_output_ / this->group_);

  // Create filter descriptor.
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int kernel_h = kernel_shape_data[0];
  const int kernel_w = kernel_shape_data[1];
  cudnn::createFilterDesc<Dtype>(&filter_desc_,
      this->num_output_ / this->group_, this->channels_ / this->group_,
      kernel_h, kernel_w);

  // Create tensor descriptor(s) for data and corresponding convolution(s).
  for (int i = 0; i < bottom.size(); i++) {
    cudnnTensorDescriptor_t bottom_desc;
    cudnn::createTensor4dDesc<Dtype>(&bottom_desc);
    bottom_descs_.push_back(bottom_desc);
    cudnnTensorDescriptor_t top_desc;
    cudnn::createTensor4dDesc<Dtype>(&top_desc);
    top_descs_.push_back(top_desc);
    cudnnConvolutionDescriptor_t conv_desc;
    cudnn::createConvolutionDesc<Dtype>(&conv_desc);
    conv_descs_.push_back(conv_desc);
  }

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::createTensor4dDesc<Dtype>(&bias_desc_);
  }
#endif

  handles_setup_ = true;
}

template <typename Dtype>
void CuDNNConvolutionLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  ConvolutionLayer<Dtype>::Reshape(bottom, top);
  CHECK_EQ(2, this->num_spatial_axes_)
      << "CuDNNConvolution input must have 2 spatial axes "
      << "(e.g., height and width). "
      << "Use 'engine: CAFFE' for general ND convolution.";
  this->bottom_offset_ = this->bottom_dim_ / this->group_;
  this->top_offset_ = this->top_dim_ / this->group_;
  const int height = bottom[0]->shape(this->channel_axis_ + 1);
  const int width = bottom[0]->shape(this->channel_axis_ + 2);
  const int height_out = top[0]->shape(this->channel_axis_ + 1);
  const int width_out = top[0]->shape(this->channel_axis_ + 2);
  const int* pad_data = this->pad_.cpu_data();
  const int pad_h = pad_data[0];
  const int pad_w = pad_data[1];
  const int* stride_data = this->stride_.cpu_data();
  const int stride_h = stride_data[0];
  const int stride_w = stride_data[1];
#if CUDNN_VERSION_MIN(8, 0, 0) && defined(USE_CUDNN_FRONTEND)
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int kernel_h = kernel_shape_data[0];
  const int kernel_w = kernel_shape_data[1];
  for (int i = 0; i < bottom.size(); i++) {
    // Note: taken from cudnn_frontend/sample/conv_sample.cpp
    // We are using 2D convs => int conv_dim = 2;
    // x = [n,c,h,w], w = [cout,cin,h,w], y = [n,c,h,w]
    int64_t x_dim[]    = {this->num_, this->channels_, height, width};
    int64_t w_dim[]    = {this->num_output_, this->channels_, kernel_h, kernel_w};
    int64_t y_dim[]    = {this->num_, this->num_output_, height_out, width_out};
    int64_t pad[]      = {pad_h, pad_w};
    int64_t dilation[] = {1, 1};
    int64_t stride[]   = {stride_h, stride_w};
    // Note: This is a hack. Running openpose will call this function twice.
    // Without it, the cache will have the wrong plan, i.e. wrong input config.
    // The first run has 16x16 input size, default value for caffe net init???
    if (!second_run_) { second_run_ = true; continue; }
    // Create an Operation Graph.
    // Note: We create a graph of conv + scale + bias ops.
    // This is a supported graph pattern, conv + bias only is not supported.
    auto opGraph = create_graph_operation(
      x_dim, w_dim, y_dim, pad, dilation, stride, 
      CUDNN_DATA_FLOAT, CUDNN_DATA_FLOAT, handle_[0], this->bias_term_
    );
    op_graph_ = &opGraph;
    // Create an execution plan and cache it.
    // Note: Saving/caching the plan without mutex lock will fail.
    // The cache function in `cudnn_frontend` uses mutex lock.
    const cudnn_frontend::ExecutionPlan *cached_plan;
    if (!plan_cache_->get_plan_from_cache(*(op_graph_), cached_plan))
    {
      auto plan = get_execution_plan(*(op_graph_), handle_[0]);
      plan_cache_->add_plan_to_cache(*(op_graph_), plan);
      workspace_size_ = plan.getWorkspaceSize();
      checkCudaErr(cudaMalloc(&(this->workspaceData), workspace_size_)); 
    }
  }
}
#else
// Note: Copied from https://github.com/Qengineering/caffe/tree/ssd/src/caffe/layers
#if CUDNN_VERSION_MIN(8, 0, 0)
  int RetCnt;
  bool found_conv_algorithm;
  size_t free_memory, total_memory;
  cudnnConvolutionFwdAlgoPerf_t     fwd_algo_pref_[4];
  cudnnConvolutionBwdDataAlgoPerf_t bwd_data_algo_pref_[4];

  //get memory sizes
  cudaMemGetInfo(&free_memory, &total_memory);
#else
  // Specify workspace limit for kernels directly until we have a
  // planning strategy and a rewrite of Caffe's GPU memory mangagement
  size_t workspace_limit_bytes = 8*1024*1024;
#endif

  for (int i = 0; i < bottom.size(); i++) {
    cudnn::setTensor4dDesc<Dtype>(&bottom_descs_[i],
        this->num_,
        this->channels_ / this->group_, height, width,
        this->channels_ * height * width,
        height * width, width, 1);
    cudnn::setTensor4dDesc<Dtype>(&top_descs_[i],
        this->num_,
        this->num_output_ / this->group_, height_out, width_out,
        this->num_output_ * this->out_spatial_dim_,
        this->out_spatial_dim_, width_out, 1);
    cudnn::setConvolutionDesc<Dtype>(&conv_descs_[i], bottom_descs_[i],
        filter_desc_, pad_h, pad_w, stride_h, stride_w);

// Note: Copied from https://github.com/Qengineering/caffe/tree/ssd/src/caffe/layers
#if CUDNN_VERSION_MIN(8, 0, 0)
    // choose forward algorithm for filter
    // in forward filter the CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED 
    // is not implemented in cuDNN 8
    CUDNN_CHECK(
      cudnnGetConvolutionForwardAlgorithm_v7(
        handle_[0], bottom_descs_[i], filter_desc_, conv_descs_[i], 
        top_descs_[i], 4, &RetCnt, fwd_algo_pref_));

    found_conv_algorithm = false;
    for(int n=0;n<RetCnt;n++){
      if (fwd_algo_pref_[n].status == CUDNN_STATUS_SUCCESS &&
          fwd_algo_pref_[n].algo != CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED &&
          fwd_algo_pref_[n].memory < free_memory){
        found_conv_algorithm = true;
        fwd_algo_[i] = fwd_algo_pref_[n].algo;
        workspace_fwd_sizes_[i] = fwd_algo_pref_[n].memory;
        break;
      }
    }
    // !!!Note: If we comment out the following, the code will run but uses a
    // slower conv algorithm with less memory footprint when no more memory 
    // is available. This means that the default values are used.
    if(!found_conv_algorithm) LOG(ERROR) << "cuDNN did not return a suitable algorithm for convolution.";
    else{
      // choose backward algorithm for filter
        // for better or worse, just a fixed constant due to the missing
        // cudnnGetConvolutionBackwardFilterAlgorithm in cuDNN version 8.0
      bwd_filter_algo_[i] = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1;
      //twice the amount of the forward search to be save
      workspace_bwd_filter_sizes_[i] = 2*workspace_fwd_sizes_[i];
    }

    // choose backward algo for data
    CUDNN_CHECK(
      cudnnGetConvolutionBackwardDataAlgorithm_v7(
        handle_[0], filter_desc_, top_descs_[i], conv_descs_[i], 
        bottom_descs_[i], 4, &RetCnt, bwd_data_algo_pref_));

    found_conv_algorithm = false;
    for(int n=0;n<RetCnt;n++){
      if (bwd_data_algo_pref_[n].status == CUDNN_STATUS_SUCCESS &&
          bwd_data_algo_pref_[n].algo != CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD &&
          bwd_data_algo_pref_[n].algo != CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED &&
          bwd_data_algo_pref_[n].memory < free_memory){
        found_conv_algorithm = true;
        bwd_data_algo_[i]              = bwd_data_algo_pref_[n].algo;
        workspace_bwd_data_sizes_[i]   = bwd_data_algo_pref_[n].memory;
        break;
      }
    }
    // !!!Note: If we comment out the following, the code will run but uses a
    // slower conv algorithm with less memory footprint when no more memory 
    // is available. This means that the default values are used.
    if(!found_conv_algorithm) LOG(ERROR) << "cuDNN did not return a suitable algorithm for convolution.";
#else
    // choose forward and backward algorithms + workspace(s)
    CUDNN_CHECK(cudnnGetConvolutionForwardAlgorithm(handle_[0],
      bottom_descs_[i],
      filter_desc_,
      conv_descs_[i],
      top_descs_[i],
      CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
      workspace_limit_bytes,
      &fwd_algo_[i]));

    CUDNN_CHECK(cudnnGetConvolutionForwardWorkspaceSize(handle_[0],
      bottom_descs_[i],
      filter_desc_,
      conv_descs_[i],
      top_descs_[i],
      fwd_algo_[i],
      &(workspace_fwd_sizes_[i])));

    // choose backward algorithm for filter
    CUDNN_CHECK(cudnnGetConvolutionBackwardFilterAlgorithm(handle_[0],
          bottom_descs_[i], top_descs_[i], conv_descs_[i], filter_desc_,
          CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
          workspace_limit_bytes, &bwd_filter_algo_[i]) );

    // get workspace for backwards filter algorithm
    CUDNN_CHECK(cudnnGetConvolutionBackwardFilterWorkspaceSize(handle_[0],
          bottom_descs_[i], top_descs_[i], conv_descs_[i], filter_desc_,
          bwd_filter_algo_[i], &workspace_bwd_filter_sizes_[i]));

    // choose backward algo for data
    CUDNN_CHECK(cudnnGetConvolutionBackwardDataAlgorithm(handle_[0],
          filter_desc_, top_descs_[i], conv_descs_[i], bottom_descs_[i],
          CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
        workspace_limit_bytes, &bwd_data_algo_[i]));

    // get workspace size
    CUDNN_CHECK(cudnnGetConvolutionBackwardDataWorkspaceSize(handle_[0],
          filter_desc_, top_descs_[i], conv_descs_[i], bottom_descs_[i],
          bwd_data_algo_[i], &workspace_bwd_data_sizes_[i]) );
#endif
  }

  // reduce over all workspace sizes to get a maximum to allocate / reallocate
  size_t total_workspace_fwd = 0;
  size_t total_workspace_bwd_data = 0;
  size_t total_workspace_bwd_filter = 0;

  for (size_t i = 0; i < bottom.size(); i++) {
    total_workspace_fwd        = std::max(total_workspace_fwd,
                                     workspace_fwd_sizes_[i]);
    total_workspace_bwd_data   = std::max(total_workspace_bwd_data,
                                     workspace_bwd_data_sizes_[i]);
    total_workspace_bwd_filter = std::max(total_workspace_bwd_filter,
                                     workspace_bwd_filter_sizes_[i]);
  }
  // get max over all operations
  size_t max_workspace = std::max(total_workspace_fwd,
                             total_workspace_bwd_data);
  max_workspace = std::max(max_workspace, total_workspace_bwd_filter);
  // ensure all groups have enough workspace
  size_t total_max_workspace = max_workspace *
                               (this->group_ * CUDNN_STREAMS_PER_GROUP);

  // this is the total amount of storage needed over all groups + streams
  if (total_max_workspace > workspaceSizeInBytes) {
    DLOG(INFO) << "Reallocating workspace storage: " << total_max_workspace;
    workspaceSizeInBytes = total_max_workspace;

    // free the existing workspace and allocate a new (larger) one
    cudaFree(this->workspaceData);

    cudaError_t err = cudaMalloc(&(this->workspaceData), workspaceSizeInBytes);
    if (err != cudaSuccess) {
      // force zero memory path
      for (int i = 0; i < bottom.size(); i++) {
        workspace_fwd_sizes_[i] = 0;
        workspace_bwd_filter_sizes_[i] = 0;
        workspace_bwd_data_sizes_[i] = 0;
        fwd_algo_[i] = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM;
        bwd_filter_algo_[i] = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_0;
        bwd_data_algo_[i] = CUDNN_CONVOLUTION_BWD_DATA_ALGO_0;
      }

      // NULL out all workspace pointers
      for (int g = 0; g < (this->group_ * CUDNN_STREAMS_PER_GROUP); g++) {
        workspace[g] = NULL;
      }
      // NULL out underlying data
      workspaceData = NULL;
      workspaceSizeInBytes = 0;
    }

    // if we succeed in the allocation, set pointer aliases for workspaces
    for (int g = 0; g < (this->group_ * CUDNN_STREAMS_PER_GROUP); g++) {
      workspace[g] = reinterpret_cast<char *>(workspaceData) + g*max_workspace;
    }
  }

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::setTensor4dDesc<Dtype>(&bias_desc_,
        1, this->num_output_ / this->group_, 1, 1);
  }
}
#endif

template <typename Dtype>
CuDNNConvolutionLayer<Dtype>::~CuDNNConvolutionLayer() {
  // Check that handles have been setup before destroying.
  if (!handles_setup_) { return; }

#if CUDNN_VERSION_MIN(8, 0, 0) && defined(USE_CUDNN_FRONTEND)
  for (int g = 0; g < this->group_ * CUDNN_STREAMS_PER_GROUP; g++) {
    cudaStreamDestroy(stream_[g]);
    cudnnDestroy(handle_[g]);
  }

  if (workspace_size_ > 0) { checkCudaErr(cudaFree(workspaceData)); }
  delete [] stream_;
  delete [] handle_;
#else
  for (int i = 0; i < bottom_descs_.size(); i++) {
    cudnnDestroyTensorDescriptor(bottom_descs_[i]);
    cudnnDestroyTensorDescriptor(top_descs_[i]);
    cudnnDestroyConvolutionDescriptor(conv_descs_[i]);
  }
  if (this->bias_term_) {
    cudnnDestroyTensorDescriptor(bias_desc_);
  }
  cudnnDestroyFilterDescriptor(filter_desc_);

  for (int g = 0; g < this->group_ * CUDNN_STREAMS_PER_GROUP; g++) {
    cudaStreamDestroy(stream_[g]);
    cudnnDestroy(handle_[g]);
  }

  cudaFree(workspaceData);
  delete [] stream_;
  delete [] handle_;
  delete [] fwd_algo_;
  delete [] bwd_filter_algo_;
  delete [] bwd_data_algo_;
  delete [] workspace_fwd_sizes_;
  delete [] workspace_bwd_data_sizes_;
  delete [] workspace_bwd_filter_sizes_;
#endif
}

INSTANTIATE_CLASS(CuDNNConvolutionLayer);

}   // namespace caffe
#endif
