#include "src/backends/torch_scripted/handler/base_handler.hh"

namespace torchserve {
  namespace torchscripted {
    std::pair<std::shared_ptr<torch::jit::script::Module>, std::shared_ptr<torch::Device>> 
    BaseHandler::LoadModel(
      std::shared_ptr<torchserve::LoadModelRequest>& load_model_request) {
      try {
        auto device = GetTorchDevice(load_model_request);
        auto module = std::make_shared<torch::jit::script::Module>(torch::jit::load(
          // TODO: windows
          fmt::format("{}/{}", 
          load_model_request->model_dir, 
          manifest_->GetModel().serialized_file),
          *device));
        return std::make_pair(module, device);
      } catch (const c10::Error& e) {
        TS_LOGF(ERROR, "loading the model: {}, device id: {}, error: {}",
                load_model_request->model_name,
                load_model_request->gpu_id,
                e.msg());
        throw e;
      } catch (const std::runtime_error& e) {
        TS_LOGF(ERROR, "loading the model: {}, device id: {}, error: {}",
                load_model_request->model_name,
                load_model_request->gpu_id,
                e.what());
        throw e;
      }
    }

    std::shared_ptr<torch::Device> BaseHandler::GetTorchDevice(
      std::shared_ptr<torchserve::LoadModelRequest>& load_model_request) {
      /**
       * @brief 
       * TODO: extend LoadModelRequest to support 
       * - device type: CPU, GPU or others
       */
      if (load_model_request->gpu_id < 0) {
        return std::make_shared<torch::Device>(torch::kCPU);
      } 

      return std::make_shared<torch::Device>(torch::kCUDA, load_model_request->gpu_id);
    }

    std::vector<torch::jit::IValue> BaseHandler::Preprocess(
      std::shared_ptr<torch::Device>& device,
      std::map<uint8_t, std::string>& idx_to_req_id,
      std::shared_ptr<torchserve::InferenceRequestBatch>& request_batch,
      std::shared_ptr<torchserve::InferenceResponseBatch>& response_batch)  {
      /**
       * @brief 
       * Ref: https://github.com/pytorch/serve/blob/master/ts/torch_handler/vision_handler.py#L27
       */
      std::vector<torch::jit::IValue> batch_ivalue;
      std::vector<torch::Tensor> batch_tensors;
      uint8_t idx = 0;
      for (auto& request : *request_batch) {
        auto data_it = request.parameters.find(torchserve::PayloadType::kPARAMETER_NAME_DATA);
        auto dtype_it = request.headers.find(torchserve::PayloadType::kHEADER_NAME_DATA_TYPE);
        if (data_it == request.parameters.end()) {
          data_it = request.parameters.find(torchserve::PayloadType::kPARAMETER_NAME_BODY);
          dtype_it = request.headers.find(torchserve::PayloadType::kHEADER_NAME_BODY_TYPE);
        }

        if (data_it == request.parameters.end() || 
          dtype_it == request.headers.end()) {
          TS_LOGF(ERROR, "Empty payload for request id: {}", request.request_id);
          auto response = (*response_batch)[request.request_id];
          response->SetResponse(500, "data_type", torchserve::PayloadType::kCONTENT_TYPE_TEXT, "Empty payload");
          continue;
        } 
        /*
        case2: the image is sent as string of bytesarray
        if (dtype_it->second == "String") {
          try {
            auto b64decoded_str = folly::base64Decode(data_it->second);
            torchserve::Converter::StrToBytes(b64decoded_str, image);
          } catch (folly::base64_decode_error e) {
            TS_LOGF(ERROR, "Failed to base64Decode for request id: {}, error: {}",
                    request.request_id,
                    e.what());
          }
        }
        */

        try {
          if (dtype_it->second == torchserve::PayloadType::kDATA_TYPE_BYTES) {
            // case2: the image is sent as bytesarray
            //torch::serialize::InputArchive archive;
            //archive.load_from(std::istringstream iss(std::string(data_it->second)));
            /*
            std::istringstream iss(std::string(data_it->second.begin(), data_it->second.end()));
            torch::serialize::InputArchive archive;
            images.emplace_back(archive.load_from(iss, torch::Device device);
            
            std::vector<char> bytes(
              static_cast<char>(*data_it->second.begin()), 
              static_cast<char>(*data_it->second.end()));
            
            images.emplace_back(torch::pickle_load(bytes).toTensor().to(*device));
            */
            batch_tensors.emplace_back(torch::pickle_load(data_it->second).toTensor().to(*device));
            idx_to_req_id[idx++] = request.request_id;
          } else if (dtype_it->second == "List") {
            // case3: the image is a list
          }
        } catch (const std::runtime_error& e) {
          TS_LOGF(ERROR, "Failed to load tensor for request id: {}, error: {}",
                  request.request_id,
                  e.what());
          auto response = (*response_batch)[request.request_id];
          response->SetResponse(
            500, 
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_STRING,
            "runtime_error, failed to load tensor");
        } catch (const c10::Error& e) {
          TS_LOGF(ERROR, "Failed to load tensor for request id: {}, c10 error: {}",
                  request.request_id,
                  e.msg());
          auto response = (*response_batch)[request.request_id];
          response->SetResponse(
            500, 
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_STRING,
            "c10 error, failed to load tensor");
        }
      }
      if (!batch_tensors.empty()) {
        batch_ivalue.emplace_back(torch::stack(batch_tensors).to(*device));
      }
      
      return batch_ivalue;
    }

    torch::Tensor BaseHandler::Inference(
      std::shared_ptr<torch::jit::script::Module> model, 
      std::vector<torch::jit::IValue>& inputs,
      std::shared_ptr<torch::Device>& device,
      std::map<uint8_t, std::string>& idx_to_req_id,
      std::shared_ptr<torchserve::InferenceResponseBatch>& response_batch) {
      if (device == nullptr) {
        TS_LOG(WARN, "device is nullptr");
      }
      try {
        torch::NoGradGuard no_grad;
        return model->forward(inputs).toTensor();
      } catch (const std::runtime_error& e) {
        TS_LOGF(ERROR, "Failed to predict, error: {}", e.what());
        for (auto& kv : idx_to_req_id) {
          auto response = (*response_batch)[kv.second];
          response->SetResponse(
            500, 
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_STRING,
            "runtime_error, failed to inference");
        }
        throw e;
      }
    }

    void BaseHandler::Postprocess(
      const torch::Tensor& data,
      std::map<uint8_t, std::string>& idx_to_req_id,
      std::shared_ptr<torchserve::InferenceResponseBatch>& response_batch) {
      for (const auto& kv : idx_to_req_id) {
        try {
          auto response = (*response_batch)[kv.second];
          auto msg = torch::argmax(data[kv.first]).to(torch::kFloat32);
          response->SetResponse(
            200,
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_BYTES,
            torch::pickle_save(data[kv.first]));
        } catch (const std::runtime_error& e) {
          TS_LOGF(ERROR, "Failed to load tensor for request id: {}, error: {}",
                  kv.second,
                  e.what());
          auto response = (*response_batch)[kv.second];
          response->SetResponse(
            500, 
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_STRING,
            "runtime_error, failed to postprocess tensor");
        } catch (const c10::Error& e) {
          TS_LOGF(ERROR, "Failed to postprocess tensor for request id: {}, error: {}",
                  kv.second,
                  e.msg());
          auto response = (*response_batch)[kv.second];
          response->SetResponse(
            500, 
            "data_type",
            torchserve::PayloadType::kDATA_TYPE_STRING,
            "c10 error, failed to postprocess tensor");
        }
      }
    }
  } // namespace torchscripted
} // namespace torchserve