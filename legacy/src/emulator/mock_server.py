import logging
import math
import struct
import time
from concurrent import futures

import grpc

import voicebot_pb2
import voicebot_pb2_grpc

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")


# 16kHz, 16bit, mono 삐-소리 생성기 (TTS 음원 모의용)
def generate_beep_audio(duration_sec=2.0, freq=440.0, sr=16000):
    samples = int(duration_sec * sr)
    audio = bytearray()
    for i in range(samples):
        t = i / float(sr)
        val = int(math.sin(2 * math.pi * freq * t) * 10000)
        audio += struct.pack("<h", val)
    return bytes(audio)


class VoicebotAiServiceServicer(voicebot_pb2_grpc.VoicebotAiServiceServicer):
    def StreamSession(self, request_iterator, context):  # [C-8 Fix] proto 메서드명 일치
        logging.info("====================================")
        logging.info("[Gateway] New SIP Call connected to AI.")

        is_user_speaking = False
        speaking_duration = 0

        try:
            for req in request_iterator:
                # Gateway의 Silero VAD가 사용자 발화를 감지해 True를 보냈는가?
                if req.is_speaking:
                    if not is_user_speaking:
                        logging.info(
                            "\n🗣️ [STT/VAD] User started speaking! (VAD On -> Trigger Barge-in)"
                        )
                        is_user_speaking = True
                        speaking_duration = 0

                        # [말끊기 시나리오] 유저 발화 감지 시, 게이트웨이의 남은 TTS 재생 버퍼를 삭제하도록 강제 오더 전송
                        yield voicebot_pb2.AiResponse(
                            type=voicebot_pb2.AiResponse.END_OF_TURN, clear_buffer=True
                        )

                    speaking_duration += len(req.audio_data)

                # 사용자가 침묵(Silence) 중이라면?
                else:
                    if is_user_speaking:
                        logging.info(
                            f"🤐 [STT/VAD] User stopped speaking. (VAD Off, Gathered {speaking_duration} bytes of voice)"
                        )
                        is_user_speaking = False

                        # (발화 종료 감지 됨) -> 모의 STT 결과 및 LLM 처리 딜레이 시뮬레이션
                        time.sleep(0.5)

                        logging.info("🤖 [AI] Sending mock STT and TTS response...")
                        # [Phase3-M5 Fix] proto 필드명 text → text_content (AiResponse.text_content)
                        yield voicebot_pb2.AiResponse(
                            type=voicebot_pb2.AiResponse.STT_RESULT,
                            text_content="네, 고객님 요청하신 사항을 처리하겠습니다. 삐 소리를 들려드릴게요.",
                        )

                        # [C-8 Fix] TTS_TEXT는 proto에 없는 enum — 제거
                        logging.info("🎵 [TTS] Streaming generated audio chunks...")
                        audio_data = generate_beep_audio(duration_sec=3.0)  # 3초짜리 오디오
                        # 3200바이트(100ms) 단위로 잘라서 리얼타임처럼 전송
                        chunk_size = 3200
                        for i in range(0, len(audio_data), chunk_size):
                            chunk = audio_data[i : i + chunk_size]
                            yield voicebot_pb2.AiResponse(
                                type=voicebot_pb2.AiResponse.TTS_AUDIO, audio_data=chunk
                            )
                            time.sleep(0.09)  # 네트워크 스트리밍 딜레이 시뮬레이션

                        logging.info("✅ [AI] End of Turn Signal Sent.\n")
                        yield voicebot_pb2.AiResponse(
                            type=voicebot_pb2.AiResponse.END_OF_TURN, clear_buffer=False
                        )
        except Exception as e:
            logging.error(f"Stream disconnected or Error: {e}")


def serve():
    # 최대 10개의 동시 통화를 에뮬레이트하는 서버스레드 풀
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    voicebot_pb2_grpc.add_VoicebotAiServiceServicer_to_server(VoicebotAiServiceServicer(), server)

    # IPv6 bind 실패 환경(샌드박스/CI) 대응: IPv4 loopback으로 폴백
    bind_port = 0
    bind_addr = ""
    try:
        bind_port = server.add_insecure_port("[::]:50051")
        bind_addr = "[::]:50051"
    except Exception as e:
        logging.warning("IPv6 bind failed for mock server: %s", e)

    if bind_port == 0:
        try:
            bind_port = server.add_insecure_port("127.0.0.1:50051")
            bind_addr = "127.0.0.1:50051"
        except Exception as e:
            logging.warning("IPv4 bind failed for mock server: %s", e)
    if bind_port == 0:
        raise RuntimeError("Failed to bind gRPC mock server to 50051 on IPv6/IPv4")

    server.start()
    logging.info("🚀 Python STT/TTS Emulator running on %s...", bind_addr)
    logging.info("Waiting for VBGW C++ Gateway calls...")
    server.wait_for_termination()


if __name__ == "__main__":
    serve()
