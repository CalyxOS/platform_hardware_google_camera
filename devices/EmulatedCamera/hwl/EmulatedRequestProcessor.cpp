/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "EmulatedRequestProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include "EmulatedRequestProcessor.h"

#include <HandleImporter.h>
#include <log/log.h>
#include <sync/sync.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::ErrorCode;
using google_camera_hal::HwlPipelineResult;
using google_camera_hal::MessageType;
using google_camera_hal::NotifyMessage;

EmulatedRequestProcessor::EmulatedRequestProcessor(uint32_t camera_id,
                                                   sp<EmulatedSensor> sensor)
    : camera_id_(camera_id),
      sensor_(sensor),
      request_state_(std::make_unique<EmulatedRequestState>(camera_id)) {
  ATRACE_CALL();
  request_thread_ = std::thread([this] { this->RequestProcessorLoop(); });
}

EmulatedRequestProcessor::~EmulatedRequestProcessor() {
  ATRACE_CALL();
  processor_done_ = true;
  request_thread_.join();

  auto ret = sensor_->ShutDown();
  if (ret != OK) {
    ALOGE("%s: Failed during sensor shutdown %s (%d)", __FUNCTION__,
          strerror(-ret), ret);
  }
}

status_t EmulatedRequestProcessor::ProcessPipelineRequests(
    uint32_t frame_number, const std::vector<HwlPipelineRequest>& requests,
    const std::vector<EmulatedPipeline>& pipelines) {
  ATRACE_CALL();

  std::unique_lock<std::mutex> lock(process_mutex_);

  for (const auto& request : requests) {
    if (request.pipeline_id >= pipelines.size()) {
      ALOGE("%s: Pipeline request with invalid pipeline id: %u", __FUNCTION__,
            request.pipeline_id);
      return BAD_VALUE;
    }

    while (pending_requests_.size() > EmulatedSensor::kPipelineDepth) {
      auto result = request_condition_.wait_for(
          lock, std::chrono::nanoseconds(
                    EmulatedSensor::kSupportedFrameDurationRange[1]));
      if (result == std::cv_status::timeout) {
        ALOGE("%s Timed out waiting for a pending request slot", __FUNCTION__);
        return TIMED_OUT;
      }
    }

    auto output_buffers = CreateSensorBuffers(
        frame_number, request.output_buffers,
        pipelines[request.pipeline_id].streams, request.pipeline_id,
        pipelines[request.pipeline_id].cb);
    auto input_buffers = CreateSensorBuffers(
        frame_number, request.input_buffers,
        pipelines[request.pipeline_id].streams, request.pipeline_id,
        pipelines[request.pipeline_id].cb);

    pending_requests_.push(
        {.settings = HalCameraMetadata::Clone(request.settings.get()),
         .input_buffers = std::move(input_buffers),
         .output_buffers = std::move(output_buffers)});
  }

  return OK;
}

std::unique_ptr<Buffers> EmulatedRequestProcessor::CreateSensorBuffers(
    uint32_t frame_number, const std::vector<StreamBuffer>& buffers,
    const std::unordered_map<uint32_t, EmulatedStream>& streams,
    uint32_t pipeline_id, HwlPipelineCallback cb) {
  if (buffers.empty()) {
    return nullptr;
  }

  auto sensor_buffers = std::make_unique<Buffers>();
  sensor_buffers->reserve(buffers.size());
  for (const auto& buffer : buffers) {
    auto sensor_buffer = CreateSensorBuffer(
        frame_number, streams.at(buffer.stream_id), pipeline_id, cb, buffer);
    if (sensor_buffer.get() != nullptr) {
      sensor_buffers->push_back(std::move(sensor_buffer));
    }
  }

  return sensor_buffers;
}

void EmulatedRequestProcessor::NotifyFailedRequest(const PendingRequest& request) {
  if (request.output_buffers->at(0)->callback.notify != nullptr) {
    NotifyMessage msg = {
        .type = MessageType::kError,
        .message.error = {
            .frame_number = request.output_buffers->at(0)->frame_number,
            .error_stream_id = -1,
            .error_code = ErrorCode::kErrorRequest}};
    request.output_buffers->at(0)->callback.notify(
        request.output_buffers->at(0)->pipeline_id, msg);
  }
}

status_t EmulatedRequestProcessor::Flush() {
  std::lock_guard<std::mutex> lock(process_mutex_);
  // First flush in-flight requests
  auto ret = sensor_->Flush();

  // Then the rest of the pending requests
  while (!pending_requests_.empty()) {
    const auto& request = pending_requests_.front();
    NotifyFailedRequest(request);
    pending_requests_.pop();
  }

  return ret;
}

status_t EmulatedRequestProcessor::GetBufferSizeAndStride(
    const EmulatedStream& stream, uint32_t* size /*out*/,
    uint32_t* stride /*out*/) {
  if (size == nullptr) {
    return BAD_VALUE;
  }

  switch (stream.override_format) {
    case HAL_PIXEL_FORMAT_RGB_888:
      *stride = stream.width * 3;
      *size = (*stride) * stream.width;
      break;
    case HAL_PIXEL_FORMAT_RGBA_8888:
      *stride = stream.width * 4;
      ;
      *size = (*stride) * stream.width;
      break;
    case HAL_PIXEL_FORMAT_Y16:
      if (stream.override_data_space == HAL_DATASPACE_DEPTH) {
        *stride = AlignTo(AlignTo(stream.width, 2) * 2, 16);
        *size = (*stride) * AlignTo(stream.height, 2);
      } else {
        return BAD_VALUE;
      }
      break;
    case HAL_PIXEL_FORMAT_BLOB:
      if (stream.override_data_space == HAL_DATASPACE_V0_JFIF) {
        *size = stream.buffer_size;
        *stride = *size;
      } else {
        return BAD_VALUE;
      }
      break;
    case HAL_PIXEL_FORMAT_RAW16:
      *stride = stream.width * 2;
      *size = (*stride) * stream.width;
      break;
    default:
      return BAD_VALUE;
  }

  return OK;
}

status_t EmulatedRequestProcessor::LockSensorBuffer(
    const EmulatedStream& stream, HandleImporter& importer /*in*/,
    buffer_handle_t buffer, SensorBuffer* sensor_buffer /*out*/) {
  if (sensor_buffer == nullptr) {
    return BAD_VALUE;
  }

  auto width = static_cast<int32_t>(stream.width);
  auto height = static_cast<int32_t>(stream.height);
  if (stream.override_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
    IMapper::Rect map_rect = {0, 0, width, height};
    auto yuv_layout =
        importer.lockYCbCr(buffer, stream.producer_usage, map_rect);
    if ((yuv_layout.y != nullptr) && (yuv_layout.cb != nullptr) &&
        (yuv_layout.cr != nullptr)) {
      sensor_buffer->plane.img_y_crcb.img_y =
          static_cast<uint8_t*>(yuv_layout.y);
      sensor_buffer->plane.img_y_crcb.img_cb =
          static_cast<uint8_t*>(yuv_layout.cb);
      sensor_buffer->plane.img_y_crcb.img_cr =
          static_cast<uint8_t*>(yuv_layout.cr);
      sensor_buffer->plane.img_y_crcb.y_stride = yuv_layout.yStride;
      sensor_buffer->plane.img_y_crcb.cbcr_stride = yuv_layout.cStride;
      sensor_buffer->plane.img_y_crcb.cbcr_step = yuv_layout.chromaStep;
    } else {
      ALOGE("%s: Failed to lock output buffer!", __FUNCTION__);
      return BAD_VALUE;
    }
  } else {
    uint32_t buffer_size = 0, stride = 0;
    auto ret = GetBufferSizeAndStride(stream, &buffer_size, &stride);
    if (ret == OK) {
      sensor_buffer->plane.img.img = static_cast<uint8_t*>(
          importer.lock(buffer, stream.producer_usage, buffer_size));
      if (sensor_buffer->plane.img.img != nullptr) {
        sensor_buffer->plane.img.stride = stride;
        sensor_buffer->plane.img.buffer_size = buffer_size;
      } else {
        ALOGE("%s: Failed to lock output buffer!", __FUNCTION__);
        return ret;
      }
    } else {
      ALOGE("%s: Unsupported pixel format: 0x%x", __FUNCTION__,
            stream.override_format);
      return BAD_VALUE;
    }
  }

  return OK;
}

std::unique_ptr<SensorBuffer> EmulatedRequestProcessor::CreateSensorBuffer(
    uint32_t frame_number, const EmulatedStream& emulated_stream,
    uint32_t pipeline_id, HwlPipelineCallback callback,
    StreamBuffer stream_buffer) {
  auto buffer = std::make_unique<SensorBuffer>();

  auto stream = emulated_stream;
  // Make sure input stream formats are correctly mapped here
  if (stream.is_input) {
    stream.override_format =
        EmulatedSensor::OverrideFormat(stream.override_format);
  }
  buffer->width = stream.width;
  buffer->height = stream.height;
  buffer->format = stream.override_format;
  buffer->dataSpace = stream.override_data_space;
  buffer->stream_buffer = stream_buffer;
  buffer->pipeline_id = pipeline_id;
  buffer->callback = callback;
  buffer->frame_number = frame_number;
  buffer->camera_id = camera_id_;
  buffer->is_input = stream.is_input;
  // In case buffer processing is successful, flip this flag accordingly
  buffer->stream_buffer.status = BufferStatus::kError;

  auto ret = LockSensorBuffer(stream, buffer->importer, stream_buffer.buffer,
                              buffer.get());
  if (ret != OK) {
    buffer.release();
    buffer = nullptr;
  }

  if (stream_buffer.acquire_fence != nullptr) {
    auto fence_status = buffer->importer.importFence(
        stream_buffer.acquire_fence, buffer->acquire_fence_fd);
    if (!fence_status) {
      ALOGE("%s: Failed importing acquire fence!", __FUNCTION__);
      buffer.release();
      buffer = nullptr;
    }
  }

  return buffer;
}

std::unique_ptr<Buffers> EmulatedRequestProcessor::AcquireBuffers(
    Buffers* buffers) {
  if ((buffers == nullptr) || (buffers->empty())) {
    return nullptr;
  }

  auto acquired_buffers = std::make_unique<Buffers>();
  acquired_buffers->reserve(buffers->size());
  auto output_buffer = buffers->begin();
  while (output_buffer != buffers->end()) {
    status_t ret = OK;
    if ((*output_buffer)->acquire_fence_fd >= 0) {
      ret = sync_wait((*output_buffer)->acquire_fence_fd,
                      ns2ms(EmulatedSensor::kSupportedFrameDurationRange[1]));
      if (ret != OK) {
        ALOGE("%s: Fence sync failed: %s, (%d)", __FUNCTION__, strerror(-ret),
              ret);
      }
    }

    if (ret == OK) {
      acquired_buffers->push_back(std::move(*output_buffer));
    }

    output_buffer = buffers->erase(output_buffer);
  }

  return acquired_buffers;
}

void EmulatedRequestProcessor::RequestProcessorLoop() {
  ATRACE_CALL();

  while (!processor_done_) {
    {
      std::lock_guard<std::mutex> lock(process_mutex_);
      if (!pending_requests_.empty()) {
        status_t ret;
        const auto& request = pending_requests_.front();
        auto frame_number = request.output_buffers->at(0)->frame_number;
        auto notify_callback = request.output_buffers->at(0)->callback;
        auto pipeline_id = request.output_buffers->at(0)->pipeline_id;
        EmulatedSensor::SensorSettings settings;

        // Repeating requests usually include valid settings only during the
        // initial call. Afterwards an invalid settings pointer means that there
        // are no changes in the parameters and Hal should re-use the last valid
        // values.
        if (request.settings.get() != nullptr) {
          ret = request_state_->InitializeSensorSettings(
              HalCameraMetadata::Clone(request.settings.get()), &settings);
          last_settings_ = HalCameraMetadata::Clone(request.settings.get());
        } else {
          ret = request_state_->InitializeSensorSettings(
              HalCameraMetadata::Clone(last_settings_.get()), &settings);
        }

        auto output_buffers = AcquireBuffers(request.output_buffers.get());
        if (!output_buffers->empty() && (ret == OK)) {
          auto result =
              request_state_->InitializeResult(pipeline_id, frame_number);
          auto input_buffers = AcquireBuffers(request.input_buffers.get());
          sensor_->SetCurrentRequest(settings, std::move(result),
                                     std::move(input_buffers),
                                     std::move(output_buffers));
        } else {
          // No further processing is needed, just fail the result which will
          // complete this request.
          NotifyMessage msg{.type = MessageType::kError,
                            .message.error = {
                                .frame_number = frame_number,
                                .error_stream_id = -1,
                                .error_code = ErrorCode::kErrorResult,
                            }};

          notify_callback.notify(pipeline_id, msg);
        }

        pending_requests_.pop();
        request_condition_.notify_one();
      }
    }

    sensor_->WaitForVSync(EmulatedSensor::kSupportedFrameDurationRange[1]);
  }
}

status_t EmulatedRequestProcessor::Initialize(
    std::unique_ptr<HalCameraMetadata> static_meta) {
  std::lock_guard<std::mutex> lock(process_mutex_);
  return request_state_->Initialize(std::move(static_meta));
}

status_t EmulatedRequestProcessor::GetDefaultRequest(
    RequestTemplate type, std::unique_ptr<HalCameraMetadata>* default_settings) {
  std::lock_guard<std::mutex> lock(process_mutex_);
  return request_state_->GetDefaultRequest(type, default_settings);
}

}  // namespace android
