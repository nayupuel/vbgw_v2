#include "VoicebotAiClient.h"

#include "../utils/AppConfig.h"
#include "../utils/RuntimeMetrics.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

using grpc::Channel;
using grpc::Status;
using voicebot::ai::AiResponse;
using voicebot::ai::AudioChunk;

// 편의상 stream 타입 alias
using Stream = grpc::ClientReaderWriter<AudioChunk, AiResponse>;

VoicebotAiClient::VoicebotAiClient(std::shared_ptr<Channel> channel)
    : stub_(voicebot::ai::VoicebotAiService::NewStub(channel)),
      is_running_(false),
      // [R-4 Fix] 재연결 정책을 AppConfig에서 읽어 초기화
      max_reconnect_retries_(AppConfig::instance().grpc_max_reconnect_retries),
      max_backoff_ms_(AppConfig::instance().grpc_max_backoff_ms)
{}

VoicebotAiClient::~VoicebotAiClient()
{
    endSession();
}

void VoicebotAiClient::setTtsCallback(std::function<void(const uint8_t*, size_t)> cb)
{
    on_tts_received_ = std::move(cb);
}

void VoicebotAiClient::setTtsClearCallback(std::function<void()> cb)
{
    on_tts_clear_ = std::move(cb);
}

void VoicebotAiClient::setErrorCallback(std::function<void(const std::string&)> cb)
{
    on_error_ = std::move(cb);
}

// [H-6 Fix] gRPC 스트림 최대 수명 deadline — AppConfig에서 캐싱된 값 사용
static std::chrono::system_clock::time_point makeStreamDeadline()
{
    int secs = AppConfig::instance().grpc_stream_deadline_secs;
    return std::chrono::system_clock::now() + std::chrono::seconds(secs);
}

const char* VoicebotAiClient::streamStateName(StreamState state)
{
    switch (state) {
        case StreamState::Idle:
            return "Idle";
        case StreamState::Starting:
            return "Starting";
        case StreamState::Streaming:
            return "Streaming";
        case StreamState::Backoff:
            return "Backoff";
        case StreamState::Reconnecting:
            return "Reconnecting";
        case StreamState::Closing:
            return "Closing";
        case StreamState::Closed:
            return "Closed";
        case StreamState::Failed:
            return "Failed";
    }
    return "Unknown";
}

void VoicebotAiClient::setStreamState(StreamState next_state, const std::string& reason)
{
    const auto prev = state_.exchange(next_state, std::memory_order_acq_rel);
    if (prev == next_state) {
        return;
    }

    if (next_state == StreamState::Failed) {
        if (!reason.empty()) {
            spdlog::error("[gRPC] State {} -> {} [session={}, reason={}]", streamStateName(prev),
                          streamStateName(next_state), current_session_id_, reason);
        } else {
            spdlog::error("[gRPC] State {} -> {} [session={}]", streamStateName(prev),
                          streamStateName(next_state), current_session_id_);
        }
        return;
    }

    if (!reason.empty()) {
        spdlog::debug("[gRPC] State {} -> {} [session={}, reason={}]", streamStateName(prev),
                      streamStateName(next_state), current_session_id_, reason);
    } else {
        spdlog::debug("[gRPC] State {} -> {} [session={}]", streamStateName(prev),
                      streamStateName(next_state), current_session_id_);
    }
}

bool VoicebotAiClient::isPermanentFailureStatus(grpc::StatusCode code)
{
    switch (code) {
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::PERMISSION_DENIED:
        case grpc::StatusCode::UNAUTHENTICATED:
        case grpc::StatusCode::FAILED_PRECONDITION:
        case grpc::StatusCode::UNIMPLEMENTED:
        case grpc::StatusCode::INTERNAL:
        case grpc::StatusCode::DATA_LOSS:
            return true;
        default:
            return false;
    }
}

void VoicebotAiClient::startSession(const std::string& session_id)
{
    bool expected = false;
    if (!is_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        spdlog::warn("[gRPC] startSession ignored because session is already running: {}",
                     current_session_id_);
        return;
    }

    current_session_id_ = session_id;
    setStreamState(StreamState::Starting, "open_stream");

    auto new_context = std::make_unique<grpc::ClientContext>();
    new_context->set_deadline(makeStreamDeadline());
    auto new_stream = stub_->StreamSession(new_context.get());
    if (!new_stream) {
        is_running_.store(false, std::memory_order_release);
        setStreamState(StreamState::Failed, "initial_stream_creation_failed");
        RuntimeMetrics::instance().incGrpcStreamErrors();
        RuntimeMetrics::instance().markGrpcUnhealthy();
        if (on_error_) {
            on_error_("Failed to create initial gRPC stream");
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        context_ = std::move(new_context);
        stream_ = std::move(new_stream);
    }

    reconnect_attempts_ = 0;
    RuntimeMetrics::instance().incGrpcActiveSessions();
    RuntimeMetrics::instance().markGrpcHealthy();
    setStreamState(StreamState::Streaming, "initial_stream_ready");
    worker_thread_ = std::thread(&VoicebotAiClient::streamWorker, this);
    read_thread_ = std::thread(&VoicebotAiClient::readWorker, this);

    spdlog::info("[gRPC] AI Stream Session started for: {}", session_id);
}

void VoicebotAiClient::sendAudio(const std::vector<uint8_t>& pcm_data, bool is_speaking)
{
    sendAudio(pcm_data.data(), pcm_data.size(), is_speaking);
}

void VoicebotAiClient::sendAudio(const uint8_t* data, size_t len, bool is_speaking)
{
    if (!is_running_.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (audio_queue_.size() >= kMaxAudioQueueSize) {
            spdlog::warn("[gRPC] Audio queue full ({} frames). Dropping frame for session: {}",
                         kMaxAudioQueueSize, current_session_id_);
            RuntimeMetrics::instance().incGrpcDroppedFrames();
            return;
        }

        AudioItem item;
        item.pcm_data = std::vector<uint8_t>(data, data + len);
        item.is_speaking = is_speaking;
        audio_queue_.push(std::move(item));
        RuntimeMetrics::instance().addGrpcQueuedFrames(1);
    }
    queue_cv_.notify_one();
}

bool VoicebotAiClient::isStreaming() const
{
    return state_.load(std::memory_order_acquire) == StreamState::Streaming;
}

void VoicebotAiClient::sendDtmf(const std::string& digit)
{
    if (digit.empty() || digit.size() > 1) {
        spdlog::warn("[gRPC] sendDtmf: invalid digit '{}' — expected single char", digit);
        return;
    }

    if (!is_running_.load(std::memory_order_acquire)) {
        spdlog::warn("[gRPC] sendDtmf: session not running — dropping digit '{}'", digit);
        return;
    }

    // 스트림 상태 사전 검증 — Streaming 상태가 아니면 드롭
    if (!isStreaming()) {
        spdlog::warn("[gRPC] sendDtmf: stream in state '{}', not Streaming — dropping digit '{}'",
                     streamStateName(state_.load()), digit);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        AudioItem item;
        item.is_dtmf = true;
        item.dtmf_digit = digit;
        audio_queue_.push(std::move(item));
        RuntimeMetrics::instance().addGrpcQueuedFrames(1);
    }
    queue_cv_.notify_one();
    spdlog::info("[gRPC] DTMF '{}' queued for session: {}", digit, current_session_id_);
}

void VoicebotAiClient::streamWorker()
{
    while (is_running_.load(std::memory_order_acquire)) {
        AudioItem item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !audio_queue_.empty() || !is_running_.load(std::memory_order_acquire);
            });

            if (!is_running_.load(std::memory_order_acquire) && audio_queue_.empty())
                break;

            item = std::move(audio_queue_.front());
            audio_queue_.pop();
            RuntimeMetrics::instance().subGrpcQueuedFrames(1);
        }

        std::shared_ptr<Stream> local_stream;
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            local_stream = stream_;
        }

        if (local_stream) {
            AudioChunk chunk;
            chunk.set_session_id(current_session_id_);

            if (item.is_dtmf) {
                // [IVR] DTMF 전용 청크 — audio_data/is_speaking 미설정
                chunk.set_dtmf_digit(item.dtmf_digit);
                spdlog::debug("[gRPC] Sending DTMF '{}' for session: {}", item.dtmf_digit,
                              current_session_id_);
            } else {
                // 일반 오디오 프레임
                chunk.set_audio_data(item.pcm_data.data(), item.pcm_data.size());
                chunk.set_is_speaking(item.is_speaking);
            }

            if (!local_stream->Write(chunk)) {
                spdlog::warn("[gRPC] Stream write failed for session: {}. Triggering reconnect.",
                             current_session_id_);
                setStreamState(StreamState::Reconnecting, "write_failed");
                RuntimeMetrics::instance().incGrpcStreamErrors();
                RuntimeMetrics::instance().markGrpcUnhealthy();
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (context_)
                    context_->TryCancel();
            }
        }
    }
    spdlog::debug("[gRPC] streamWorker exited for session: {}", current_session_id_);
}

void VoicebotAiClient::endSession()
{
    // 이미 정지된 상태라면 중복 수행 방지
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        return;

    setStreamState(StreamState::Closing, "end_session");
    queue_cv_.notify_all();

    // [H-2 Fix] endSession() 스트림 정리 개선
    // TryCancel() 후 stream은 이미 CANCELLED 상태이므로
    // WritesDone()/Finish() 호출은 불필요하며 블로킹될 수 있음
    bool was_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (context_) {
            context_->TryCancel();
            was_cancelled = true;
        }
    }

    if (worker_thread_.joinable()) {
        if (worker_thread_.get_id() == std::this_thread::get_id()) {
            worker_thread_.detach();
        } else {
            worker_thread_.join();
        }
    }
    if (read_thread_.joinable()) {
        if (read_thread_.get_id() == std::this_thread::get_id()) {
            read_thread_.detach();
        } else {
            read_thread_.join();
        }
    }

    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending = audio_queue_.size();
        while (!audio_queue_.empty()) {
            audio_queue_.pop();
        }
    }
    if (pending > 0) {
        RuntimeMetrics::instance().subGrpcQueuedFrames(pending);
    }

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) {
            // [H-2 Fix] TryCancel 된 스트림에는 WritesDone/Finish를 호출하지 않음
            // — gRPC 내부적으로 블로킹되거나 무의미한 에러를 반환할 수 있음
            if (!was_cancelled) {
                try {
                    stream_->WritesDone();
                    Status status = stream_->Finish();
                    if (status.ok()) {
                        spdlog::info("[gRPC] Stream closed successfully for session: {}",
                                     current_session_id_);
                    } else {
                        spdlog::warn("[gRPC] Stream finished with status: {}",
                                     status.error_message());
                    }
                } catch (...) {
                    spdlog::warn("[gRPC] Exception during stream cleanup for session: {}",
                                 current_session_id_);
                }
            } else {
                spdlog::debug("[gRPC] Stream was cancelled — skipping WritesDone/Finish for: {}",
                              current_session_id_);
            }
            stream_.reset();
        }
        context_.reset();
    }

    RuntimeMetrics::instance().decGrpcActiveSessions();
    setStreamState(StreamState::Closed, "session_ended");
}

VoicebotAiClient::ReadOutcome VoicebotAiClient::tryConnectAndRead()
{
    std::shared_ptr<Stream> local_stream;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        local_stream = stream_;
    }

    if (!local_stream) {
        return {ReadOutcomeKind::Reconnect,
                Status(grpc::StatusCode::UNAVAILABLE, "stream pointer is null")};
    }

    AiResponse response;
    while (is_running_.load(std::memory_order_acquire) && local_stream->Read(&response)) {
        RuntimeMetrics::instance().markGrpcHealthy();
        if (response.type() == AiResponse::TTS_AUDIO) {
            const std::string& audio = response.audio_data();
            if (on_tts_received_ && !audio.empty()) {
                on_tts_received_(reinterpret_cast<const uint8_t*>(audio.data()), audio.size());
            }
        } else if (response.type() == AiResponse::STT_RESULT) {
            // [S-3 Fix] STT 결과는 debug 레벨로 하향 — 고객 발화 텍스트는 PII 포함 가능성
            spdlog::debug("[AI STT] Session {}: [REDACTED {} chars]", current_session_id_,
                          response.text_content().size());
        } else if (response.type() == AiResponse::END_OF_TURN) {
            if (response.clear_buffer() && on_tts_clear_) {
                spdlog::warn("🚨 [Barge-In] Flushed Gateway TTS RingBuffer! Session: {}",
                             current_session_id_);
                RuntimeMetrics::instance().incBargeInEvents();
                on_tts_clear_();
            }
        }
        // 정상 수신마다 재연결 카운터 초기화
        reconnect_attempts_ = 0;
    }

    if (!is_running_.load(std::memory_order_acquire)) {
        return {ReadOutcomeKind::Stopped, Status::OK};
    }

    Status status = local_stream->Finish();
    if (status.ok()) {
        return {ReadOutcomeKind::Completed, status};
    }

    RuntimeMetrics::instance().markGrpcUnhealthy();
    if (isPermanentFailureStatus(status.error_code())) {
        return {ReadOutcomeKind::PermanentFailure, status};
    }

    return {ReadOutcomeKind::Reconnect, status};
}

void VoicebotAiClient::readWorker()
{
    while (is_running_.load(std::memory_order_acquire)) {
        ReadOutcome outcome = tryConnectAndRead();

        if (!is_running_.load(std::memory_order_acquire))
            break;

        if (outcome.kind == ReadOutcomeKind::Stopped ||
            outcome.kind == ReadOutcomeKind::Completed) {
            setStreamState(StreamState::Closed, "remote_stream_completed");
            break;
        }

        if (outcome.kind == ReadOutcomeKind::PermanentFailure) {
            setStreamState(StreamState::Failed, "permanent_stream_failure");
            RuntimeMetrics::instance().incGrpcStreamErrors();
            RuntimeMetrics::instance().markGrpcUnhealthy();
            is_running_.store(false, std::memory_order_release);
            queue_cv_.notify_all();
            if (on_error_) {
                on_error_(std::string("gRPC permanent error [code=") +
                          std::to_string(static_cast<int>(outcome.status.error_code())) +
                          "]: " + outcome.status.error_message());
            }
            return;
        }

        if (reconnect_attempts_ >= max_reconnect_retries_) {
            spdlog::error("[gRPC] Max reconnect retries ({}) exceeded. Triggering error callback.",
                          max_reconnect_retries_);
            setStreamState(StreamState::Failed, "max_retries_exceeded");
            RuntimeMetrics::instance().incGrpcStreamErrors();
            RuntimeMetrics::instance().markGrpcUnhealthy();
            is_running_.store(false, std::memory_order_release);
            queue_cv_.notify_all();
            if (on_error_) {
                on_error_(std::string("gRPC stream disconnected after retries [last_code=") +
                          std::to_string(static_cast<int>(outcome.status.error_code())) +
                          "]: " + outcome.status.error_message());
            }
            return;
        }

        // [R-1 Fix] 지수 백오프: 500ms * 2^n (최대 max_backoff_ms_, 콜봇 특화 기본값 4초)
        int wait_ms = std::min(500 * (1 << reconnect_attempts_.load()), max_backoff_ms_);
        reconnect_attempts_++;
        setStreamState(StreamState::Backoff, "reconnect_backoff");
        RuntimeMetrics::instance().incGrpcReconnectAttempts();
        spdlog::warn("[gRPC] Stream disconnected [code={} msg='{}']. Reconnecting in {}ms "
                     "(attempt {}/{})",
                     static_cast<int>(outcome.status.error_code()), outcome.status.error_message(),
                     wait_ms, reconnect_attempts_.load(), max_reconnect_retries_);

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        if (!is_running_.load(std::memory_order_acquire)) {
            break;
        }

        setStreamState(StreamState::Reconnecting, "create_new_stream");
        auto new_context = std::make_unique<grpc::ClientContext>();
        new_context->set_deadline(makeStreamDeadline());
        auto new_stream = stub_->StreamSession(new_context.get());

        if (!new_stream) {
            spdlog::error("[gRPC] Failed to create new stream. Will retry until retry budget "
                          "exhausted.");
            RuntimeMetrics::instance().incGrpcStreamErrors();
            RuntimeMetrics::instance().markGrpcUnhealthy();
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            context_ = std::move(new_context);
            stream_ = std::move(new_stream);
        }

        // [R-1 Fix] 재연결 성공 시 큐 비우기 — 오래된 오디오가 AI에 전달되면 STT 결과 부정확
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            size_t flushed = audio_queue_.size();
            while (!audio_queue_.empty()) {
                audio_queue_.pop();
            }
            if (flushed > 0) {
                RuntimeMetrics::instance().subGrpcQueuedFrames(flushed);
                spdlog::info("[gRPC] Flushed {} stale audio frames after reconnection", flushed);
            }
        }

        reconnect_attempts_ = 0;
        RuntimeMetrics::instance().markGrpcHealthy();
        setStreamState(StreamState::Streaming, "reconnected");
        spdlog::info("[gRPC] Reconnected stream for session: {}", current_session_id_);
    }
    if (state_.load(std::memory_order_acquire) != StreamState::Failed) {
        setStreamState(StreamState::Closed, "read_worker_exit");
    }
    spdlog::debug("[gRPC] readWorker exited for session: {}", current_session_id_);
}
