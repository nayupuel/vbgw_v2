#pragma once
#include "voicebot.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class VoicebotAiClient
{
public:
    VoicebotAiClient(std::shared_ptr<grpc::Channel> channel);
    ~VoicebotAiClient();

    // AI 수신 (TTS 버퍼) 콜백 등록
    void setTtsCallback(std::function<void(const uint8_t*, size_t)> cb);

    // Barge-in (말끊기) 플러시 콜백 등록
    void setTtsClearCallback(std::function<void()> cb);

    // AI 스트림 끊김(장애/종료) 에러 콜백 등록
    void setErrorCallback(std::function<void(const std::string&)> cb);

    // 세션(통화) 시작 시 양방향 스트리밍 파이프 오픈
    void startSession(const std::string& session_id);

    // RTP에서 추출한 PCM 오디오 프레임 전송 (Rx -> STT)
    void sendAudio(const std::vector<uint8_t>& pcm_data, bool is_speaking);

    // [P-2 Fix] 포인터 오버로드 — 호출측 벡터 복사 없이 직접 전달
    void sendAudio(const uint8_t* data, size_t len, bool is_speaking);

    // 세션 종료
    void endSession();

private:
    enum class StreamState
    {
        Idle = 0,
        Starting,
        Streaming,
        Backoff,
        Reconnecting,
        Closing,
        Closed,
        Failed,
    };

    enum class ReadOutcomeKind
    {
        Stopped = 0,
        Completed,
        Reconnect,
        PermanentFailure,
    };

    struct ReadOutcome
    {
        ReadOutcomeKind kind;
        grpc::Status status;
    };

    void streamWorker();              // 비동기 워커 스레드 (Tx)
    void readWorker();                // 비동기 수신 스레드 (Rx) - 지수 백오프 재연결 포함
    ReadOutcome tryConnectAndRead();  // 재연결을 포함한 실제 스트리밍 수행
    void setStreamState(StreamState next_state, const std::string& reason = "");
    static const char* streamStateName(StreamState state);
    static bool isPermanentFailureStatus(grpc::StatusCode code);

    std::unique_ptr<voicebot::ai::VoicebotAiService::Stub> stub_;

    // [C-2 Fix] context_를 unique_ptr로 변경 — 재연결 시 새 인스턴스로 교체 가능
    // grpc::ClientContext는 TryCancel() 이후 재사용 불가
    std::unique_ptr<grpc::ClientContext> context_;

    // [C-1 Fix] stream_ 포인터 접근을 stream_mutex_로 보호
    // gRPC ClientReaderWriter는 서로 다른 스레드의 동시 Read/Write는 허용하지만,
    // 포인터 재할당(재연결/종료)과의 동시 접근은 UB — shared_ptr 로컬 복사로 해결
    mutable std::mutex stream_mutex_;
    std::shared_ptr<grpc::ClientReaderWriter<voicebot::ai::AudioChunk, voicebot::ai::AiResponse>>
        stream_;

    std::string current_session_id_;
    std::function<void(const uint8_t*, size_t)> on_tts_received_;
    std::function<void()> on_tts_clear_;
    std::function<void(const std::string&)> on_error_;

    // 비동기 큐 관리용
    struct AudioItem
    {
        std::vector<uint8_t> pcm_data;
        bool is_speaking;
    };
    std::queue<AudioItem> audio_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::atomic<bool> is_running_;
    std::atomic<StreamState> state_{StreamState::Idle};
    std::thread worker_thread_;
    std::thread read_thread_;

    // [R-4 Fix] 재연결 정책 — AppConfig에서 동적 설정
    int max_reconnect_retries_;
    int max_backoff_ms_;
    std::atomic<int> reconnect_attempts_{0};

    static constexpr size_t kMaxAudioQueueSize = 200;
};
