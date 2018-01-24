//
// Copyright (c) 2017 XiaoMi All rights reserved.
//

#include "mace/kernels/opencl/helper.h"
#include "mace/utils/utils.h"
#include "mace/utils/tuner.h"

namespace mace {
namespace kernels {

// [(c+3)/4*W, N * H]
void CalInOutputImageShape(const std::vector<index_t> &shape, /* NHWC */
                        std::vector<size_t> &image_shape) {
  MACE_CHECK(shape.size() == 4);
  image_shape.resize(2);
  image_shape[0] = RoundUpDiv4(shape[3]) * shape[2];
  image_shape[1] = shape[0] * shape[1];
}

// [H * W * RoundUp<4>(Ic), (Oc + 3) / 4]
void CalFilterImageShape(const std::vector<index_t> &shape, /* HWIO*/
                         std::vector<size_t> &image_shape) {
  MACE_CHECK(shape.size() == 4);
  image_shape.resize(2);
  image_shape[0] = shape[0] * shape[1] * RoundUp<index_t>(shape[2], 4);
  image_shape[1] = RoundUpDiv4(shape.back());
}

// [(size + 3) / 4, 1]
void CalArgImageShape(const std::vector<index_t> &shape,
                      std::vector<size_t> &image_shape) {
  MACE_CHECK(shape.size() == 1);
  image_shape.resize(2);
  image_shape[0] = RoundUpDiv4(shape[0]);
  image_shape[1] = 1;
}

void CalImage2DShape(const std::vector<index_t> &shape, /* NHWC */
                     const BufferType type,
                     std::vector<size_t> &image_shape) {
  switch (type) {
    case FILTER:
      CalFilterImageShape(shape, image_shape);
      break;
    case IN_OUT:
      CalInOutputImageShape(shape, image_shape);
      break;
    case ARGUMENT:
      CalArgImageShape(shape, image_shape);
      break;
    default:
      LOG(FATAL) << "Mace not supported yet.";
  }
}


std::string DtToCLDt(const DataType dt) {
  switch (dt) {
    case DT_FLOAT:
      return "float";
    case DT_HALF:
      return "half";
    default:
      LOG(FATAL) << "Unsupported data type";
      return "";
  }
}

std::string DtToCLCMDDt(const DataType dt) {
  switch (dt) {
    case DT_FLOAT:
      return "f";
    case DT_HALF:
      return "h";
    default:
      LOG(FATAL) << "Not supported data type for opencl cmd data type";
      return "";
  }
}

std::string DtToUpstreamCLDt(const DataType dt) {
  switch (dt) {
    case DT_FLOAT:
    case DT_HALF:
      return "float";
    default:
      LOG(FATAL) << "Unsupported data type";
      return "";
  }
}

std::string DtToUpstreamCLCMDDt(const DataType dt) {
  switch (dt) {
    case DT_FLOAT:
    case DT_HALF:
      return "f";
    default:
      LOG(FATAL) << "Not supported data type for opencl cmd data type";
      return "";
  }
}


void TuningOrRun3DKernel(cl::Kernel &kernel,
                         const std::string tuning_key,
                         const uint32_t *gws,
                         std::vector<uint32_t> &lws,
                         StatsFuture *future) {
  auto runtime = OpenCLRuntime::Global();
  const uint32_t kwg_size = runtime->GetKernelMaxWorkGroupSize(kernel);
  auto params_generator = [&]() -> std::vector<std::vector<uint32_t>> {
    std::vector<uint32_t> local_ws(3, 0);
    local_ws[0] = std::min<uint32_t>(gws[0], kwg_size);
    local_ws[1] = std::min<uint32_t>(gws[1], kwg_size / local_ws[0]);
    local_ws[2] = std::min<uint32_t>(gws[2],
                                     kwg_size / (local_ws[0] * local_ws[1]));
    return {
        {local_ws[0], local_ws[1], local_ws[2], 1},
        {kwg_size / 16, 4, 4, 1},
        {kwg_size / 32, 4, 8, 1},
        {kwg_size / 32, 8, 4, 1},
        {kwg_size / 64, 8, 8, 1},
        {kwg_size / 64, 16, 4, 1},
        {kwg_size / 128, 8, 16, 1},
        {kwg_size / 128, 16, 8, 1},
        {kwg_size / 128, 32, 4, 1},
        {1, kwg_size / 32, 32, 1},
        {1, kwg_size / 64, 64, 1},
        {1, kwg_size / 128, 128, 1},
        {3, 15, 9, 1},
        {7, 15, 9, 1},
        {9, 7, 15, 1},
        {15, 7, 9, 1},
        {1, kwg_size, 1, 1},
        {4, 15, 8, 1},  // SNPE size
    };
  };
  cl::Event event;
  auto func = [&](std::vector<uint32_t> &params, Timer *timer) -> cl_int {
    cl_int error = CL_SUCCESS;
    if (timer == nullptr) {
      uint32_t num_blocks = params.back();
      const uint32_t block_size = gws[2] / num_blocks;
      if (gws[2] % num_blocks > 0) num_blocks++;
      for (uint32_t i = 0; i < num_blocks; ++i) {
        uint32_t gws2 = (i == num_blocks - 1) ? (gws[2] - (i * block_size)) : block_size;
        error = runtime->command_queue().enqueueNDRangeKernel(
            kernel,
            cl::NDRange(0, 0, i * block_size),
            cl::NDRange(gws[0], gws[1], gws2),
            cl::NDRange(params[0], params[1], params[2]), nullptr, &event);
        MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
      }
    } else {
      timer->StartTiming();
      error = runtime->command_queue().enqueueNDRangeKernel(
          kernel, cl::NullRange, cl::NDRange(gws[0], gws[1], gws[2]),
          cl::NDRange(params[0], params[1], params[2]), nullptr, &event);
      MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
      timer->StopTiming();
      double elapse_time = timer->ElapsedMicros();
      timer->ClearTiming();
      uint32_t num_blocks = std::min(static_cast<uint32_t>(elapse_time / kMaxKernelExeTime) + 1, gws[2]);
      params.back() = num_blocks;
      const uint32_t block_size = gws[2] / num_blocks;
      if (gws[2] % num_blocks > 0) num_blocks++;
      for (uint32_t i = 0; i < num_blocks; ++i) {
        uint32_t gws2 = (i == num_blocks - 1) ? (gws[2] - (i * block_size)) : block_size;
        error = runtime->command_queue().enqueueNDRangeKernel(
            kernel,
            cl::NDRange(0, 0, i * block_size),
            cl::NDRange(gws[0], gws[1], gws2),
            cl::NDRange(params[0], params[1], params[2]), nullptr, &event);
        MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
        timer->AccumulateTiming();
      }
    }
    return error;
  };
  OpenCLProfilingTimer timer(&event);
  Tuner<uint32_t>::Get()->template TuneOrRun<cl_int>(
      tuning_key, lws, params_generator, func, &timer);

  if (future != nullptr) {
    future->wait_fn = [event](CallStats *stats) {
      event.wait();
      if (stats != nullptr) {
        OpenCLRuntime::Global()->GetCallStats(event, stats);
      }
    };
  }
}

void TuningOrRun2DKernel(cl::Kernel &kernel,
                         const std::string tuning_key,
                         const uint32_t *gws,
                         std::vector<uint32_t> &lws,
                         StatsFuture *future) {
  auto runtime = OpenCLRuntime::Global();
  const uint32_t kwg_size = runtime->GetKernelMaxWorkGroupSize(kernel);
  auto params_generator = [&]() -> std::vector<std::vector<uint32_t>> {
    uint32_t local_ws[2];
    local_ws[0] = std::min<uint32_t>(gws[0], kwg_size);
    local_ws[1] = std::min<uint32_t>(gws[1], kwg_size / local_ws[0]);
    return {{local_ws[0], local_ws[1], 1},
            {local_ws[1], local_ws[0], 1},
            {kwg_size / 4, 4, 1},
            {kwg_size / 16, 16, 1},
            {kwg_size / 32, 32, 1},
            {kwg_size / 64, 64, 1},
            {kwg_size / 128, 128, 1},
            {kwg_size / 256, 256, 1},
            {kwg_size / 512, 512, 1},
            {kwg_size, 1, 1},
            {1, kwg_size, 1}
    };
  };
  cl::Event event;
  auto func = [&](std::vector<uint32_t> &params, Timer *timer) -> cl_int {
    cl_int error = CL_SUCCESS;
    if (timer == nullptr) {
      uint32_t num_blocks = params.back();
      const uint32_t block_size = gws[1] / num_blocks;
      if (gws[1] % num_blocks > 0) num_blocks++;
      for (uint32_t i = 0; i < num_blocks; ++i) {
        uint32_t gws1 = (i == num_blocks - 1) ? (gws[1] - (i * block_size)) : block_size;
        error = runtime->command_queue().enqueueNDRangeKernel(
            kernel,
            cl::NDRange(0, i * block_size),
            cl::NDRange(gws[0], gws1),
            cl::NDRange(params[0], params[1]),
            nullptr, &event);
        MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
      }
    } else {
      timer->StartTiming();
      error = runtime->command_queue().enqueueNDRangeKernel(
          kernel, cl::NullRange,
          cl::NDRange(gws[0], gws[1]),
          cl::NDRange(params[0], params[1]), nullptr, &event);
      MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
      timer->StopTiming();
      double elapse_time = timer->ElapsedMicros();
      timer->ClearTiming();
      uint32_t num_blocks = std::min(static_cast<uint32_t>(elapse_time / kMaxKernelExeTime) + 1, gws[1]);
      params.back() = num_blocks;
      const uint32_t block_size = gws[1] / num_blocks;
      if (gws[1] % num_blocks > 0) num_blocks++;
      for (uint32_t i = 0; i < num_blocks; ++i) {
        uint32_t gws1 = (i == num_blocks - 1) ? (gws[1] - (i * block_size)) : block_size;
        error = runtime->command_queue().enqueueNDRangeKernel(
            kernel,
            cl::NDRange(0, i * block_size),
            cl::NDRange(gws[0], gws1),
            cl::NDRange(params[0], params[1]), nullptr, &event);
        MACE_CHECK(error == CL_SUCCESS) << "Error code: " << error;
        timer->AccumulateTiming();
      }
    }
    return error;
  };
  OpenCLProfilingTimer timer(&event);
  Tuner<uint32_t>::Get()->template TuneOrRun<cl_int>(tuning_key,
                                                     lws,
                                                     params_generator,
                                                     func,
                                                     &timer);
  if (future != nullptr) {
    future->wait_fn = [runtime, event](CallStats *stats) {
      event.wait();
      if (stats != nullptr) {
        runtime->GetCallStats(event, stats);
      }
    };
  }

}

}  // namespace kernels
}  // namespace mace
