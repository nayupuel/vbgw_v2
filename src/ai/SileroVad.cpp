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

    // [VAD-v5 Fix] Silero VAD v5 호환 — h/c 분리 → 단일 state 텐서 (2, 1, 128)
    // v4: h_state(2,1,64) + c_state(2,1,64) 별도 입력
    // v5: state(2,1,128) 통합 입력/출력
    std::vector<float> state;
    std::vector<float> input_tensor_values;
    int64_t sr = 16000;

    // [VAD-v5 Fix] ONNX 노드 이름 — 실제 모델 inspect 결과 반영
    // v4: input={"input","sr","h","c"}, output={"output","hn","cn"}
    // v5: input={"input","state","sr"}, output={"output","stateN"}
    std::vector<const char*> input_node_names = {"input", "state", "sr"};
    std::vector<const char*> output_node_names = {"output", "stateN"};

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
        // [VAD-v5 Fix] state 크기: 2 * 1 * 128 = 256 (v4는 2*1*64=128)
        state.assign(2 * 1 * 128, 0.0f);
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

    // [VAD-v5 Fix] 2. state tensor (2, 1, 128) — v4의 h/c 분리 대신 통합
    std::vector<int64_t> state_shape = {2, 1, 128};
    Ort::Value state_tensor =
        Ort::Value::CreateTensor<float>(memory_info, pimpl_->state.data(), pimpl_->state.size(),
                                        state_shape.data(), state_shape.size());

    // 3. sr tensor (scalar)
    Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(memory_info, &pimpl_->sr, 1,
                                                             (std::vector<int64_t>{1}).data(), 1);

    // [VAD-v5 Fix] 입력 순서: input, state, sr (모델 inspect 결과 기준)
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(input_tensor));
    inputs.push_back(std::move(state_tensor));
    inputs.push_back(std::move(sr_tensor));

    // [Phase2-H2 Fix] ONNX Run() 예외 처리 — 미처리 시 PJSIP 콜백 스레드 크래시
    try {
        auto output_tensors = pimpl_->session->Run(
            Ort::RunOptions{nullptr}, pimpl_->input_node_names.data(), inputs.data(), inputs.size(),
            pimpl_->output_node_names.data(), pimpl_->output_node_names.size());

        // [VAD-v5 Fix] 통합 state 업데이트 (출력 인덱스 1: stateN)
        float* state_out = output_tensors[1].GetTensorMutableData<float>();
        std::memcpy(pimpl_->state.data(), state_out, pimpl_->state.size() * sizeof(float));

        // 결과 점수 획득 (출력 인덱스 0: output)
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
