#include "SileroVad.h"

#include "../utils/RuntimeMetrics.h"

#include <onnxruntime_cxx_api.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <stdexcept>

struct SileroVad::Impl
{
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SileroVAD"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    // VAD State Tensors (2, 1, 64) - Silero VAD v4 호환
    std::vector<float> h_state;
    std::vector<float> c_state;
    std::vector<float> input_tensor_values;
    int64_t sr = 16000;

    // ONNX 노드 이름
    std::vector<const char*> input_node_names = {"input", "sr", "h", "c"};
    std::vector<const char*> output_node_names = {"output", "hn", "cn"};

    Impl(const std::string& model_path)
    {
        if (!std::filesystem::exists(model_path)) {
            throw std::runtime_error("Silero VAD 모델을 찾을 수 없습니다: " + model_path);
        }

        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
        resetStates();
    }

    void resetStates()
    {
        h_state.assign(2 * 1 * 64, 0.0f);
        c_state.assign(2 * 1 * 64, 0.0f);
    }
};

SileroVad::SileroVad()
{
    // [M-5 Fix] 모델 경로를 환경변수에서 읽음 — 컨테이너 환경 유연성 확보
    const char* model_path_env = std::getenv("SILERO_VAD_MODEL_PATH");
    std::string model_path = model_path_env ? model_path_env : "models/silero_vad.onnx";
    spdlog::info("[VAD] Loading Silero VAD model from: {}", model_path);
    pimpl_ = std::make_unique<Impl>(model_path);
}
SileroVad::~SileroVad() = default;

void SileroVad::resetState()
{
    // [M-5 Fix] resetState()도 mutex 보호 — 통화 종료/재시작 경합 방지
    std::lock_guard<std::mutex> lock(vad_mutex_);
    pimpl_->resetStates();
    last_speaking_state_ = false;
    pcm_buffer_.clear();
    pcm_head_ = 0;
}

bool SileroVad::isSpeaking(const std::vector<int16_t>& pcm, float threshold)
{
    std::lock_guard<std::mutex> lock(vad_mutex_);
    if (pcm.empty())
        return last_speaking_state_;
    return isSpeakingImpl(pcm.data(), pcm.size(), threshold);
}

bool SileroVad::isSpeaking(const int16_t* data, size_t count, float threshold)
{
    // [P-1 Fix] 포인터 오버로드 — 호출측 벡터 복사 없이 직접 처리
    std::lock_guard<std::mutex> lock(vad_mutex_);
    if (!data || count == 0)
        return last_speaking_state_;
    return isSpeakingImpl(data, count, threshold);
}

bool SileroVad::isSpeakingImpl(const int16_t* data, size_t count, float threshold)
{
    // vad_mutex_ 보유 가정
    const bool prev_state = last_speaking_state_;

    // [P-2 Fix] pcm_buffer_ 무한 성장 방지 — 최대 16384 샘플(1초 분량) 상한
    static constexpr size_t kMaxPcmBufferSize = 16384;
    if (pcm_buffer_.size() + count > kMaxPcmBufferSize) {
        // 강제 compact 후에도 초과하면 이전 데이터 버림
        if (pcm_head_ > 0) {
            pcm_buffer_.erase(pcm_buffer_.begin(),
                              pcm_buffer_.begin() + static_cast<std::ptrdiff_t>(pcm_head_));
            pcm_head_ = 0;
        }
        if (pcm_buffer_.size() + count > kMaxPcmBufferSize) {
            pcm_buffer_.clear();
            pcm_head_ = 0;
            spdlog::warn("[VAD] pcm_buffer_ overflow — forced reset ({} samples dropped)", count);
        }
    }

    pcm_buffer_.insert(pcm_buffer_.end(), data, data + count);

    // 512 샘플(32ms) 미만이면 마지막 상태 유지
    if ((pcm_buffer_.size() - pcm_head_) < 512) {
        return last_speaking_state_;
    }

    // [P-1 Fix] chunk 벡터 복사 제거 — 버퍼 내 포인터 직접 참조
    const int16_t* chunk_ptr = pcm_buffer_.data() + pcm_head_;
    pcm_head_ += 512;

    // 16비트 PCM 정수를 float32(-1.0 ~ 1.0) 텐서 포맷으로 정규화 변환
    // input_tensor_values는 Impl에 미리 할당 — 반복 heap alloc 없음
    pimpl_->input_tensor_values.resize(512);
    for (size_t i = 0; i < 512; ++i) {
        pimpl_->input_tensor_values[i] = static_cast<float>(chunk_ptr[i]) / 32768.0f;
    }

    // [P-1 Fix] 주기적 compact — 1024 offset마다 한 번 (빈번한 O(N) erase 제거)
    // float 변환 완료 후 compact해야 chunk_ptr 무효화 방지
    if (pcm_head_ >= 1024) {
        pcm_buffer_.erase(pcm_buffer_.begin(),
                          pcm_buffer_.begin() + static_cast<std::ptrdiff_t>(pcm_head_));
        pcm_head_ = 0;
    }

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // 1. input tensor (1, 512)
    std::vector<int64_t> input_shape = {1, 512};
    Ort::Value input_tensor =
        Ort::Value::CreateTensor<float>(memory_info, pimpl_->input_tensor_values.data(), 512,
                                        input_shape.data(), input_shape.size());

    // 2. sr tensor (1)
    std::vector<int64_t> sr_shape = {1};
    Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(memory_info, &pimpl_->sr, 1,
                                                             sr_shape.data(), sr_shape.size());

    // 3. h_state tensor (2, 1, 64)
    std::vector<int64_t> state_shape = {2, 1, 64};
    Ort::Value h_tensor =
        Ort::Value::CreateTensor<float>(memory_info, pimpl_->h_state.data(), pimpl_->h_state.size(),
                                        state_shape.data(), state_shape.size());

    // 4. c_state tensor (2, 1, 64)
    Ort::Value c_tensor =
        Ort::Value::CreateTensor<float>(memory_info, pimpl_->c_state.data(), pimpl_->c_state.size(),
                                        state_shape.data(), state_shape.size());

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(input_tensor));
    inputs.push_back(std::move(sr_tensor));
    inputs.push_back(std::move(h_tensor));
    inputs.push_back(std::move(c_tensor));

    // [Phase2-H2 Fix] ONNX Run() 예외 처리 — 미처리 시 PJSIP 콜백 스레드 크래시
    try {
        auto output_tensors =
            pimpl_->session->Run(Ort::RunOptions{nullptr}, pimpl_->input_node_names.data(),
                                 inputs.data(), inputs.size(), pimpl_->output_node_names.data(), 3);

        // 내부 상태(h, c) 업데이트 복사
        float* hn = output_tensors[1].GetTensorMutableData<float>();
        std::memcpy(pimpl_->h_state.data(), hn, pimpl_->h_state.size() * sizeof(float));

        float* cn = output_tensors[2].GetTensorMutableData<float>();
        std::memcpy(pimpl_->c_state.data(), cn, pimpl_->c_state.size() * sizeof(float));

        // 결과 점수 획득
        float* output = output_tensors[0].GetTensorMutableData<float>();
        last_speaking_state_ = output[0] > threshold;
    } catch (const Ort::Exception& e) {
        // 추론 실패 시 마지막 상태 유지 — 연속 통화 안정성 우선
        spdlog::error("[VAD] ONNX inference error: {} — returning last state", e.what());
    }

    if (last_speaking_state_ && !prev_state) {
        RuntimeMetrics::instance().incVadSpeechEvents();
    }

    return last_speaking_state_;
}
