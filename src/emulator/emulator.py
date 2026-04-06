import asyncio
import os
import time
import wave

import grpc

import voicebot_pb2
import voicebot_pb2_grpc


class VoicebotAiEmulator(voicebot_pb2_grpc.VoicebotAiServiceServicer):
    def __init__(self, tts_file):
        self.tts_file = tts_file

    async def StreamSession(self, request_iterator, context):
        print("\n[Emulator] New Session Started.")
        session_id = "UNKNOWN"
        is_barged_in = False

        tts_audio = b""
        try:
            with wave.open(self.tts_file, "rb") as wf:
                tts_audio = wf.readframes(wf.getnframes())
        except Exception as e:
            print(f"[Emulator] Warning: Could not read TTS file {self.tts_file}: {e}")

        async def process_incoming():
            nonlocal session_id, is_barged_in
            input_audio_frames = bytearray()
            try:
                async for chunk in request_iterator:
                    if session_id == "UNKNOWN" and chunk.session_id:
                        session_id = chunk.session_id
                        print(f"[Emulator] Session ID mapped to: {session_id}")

                    input_audio_frames.extend(chunk.audio_data)

                    if chunk.is_speaking and not is_barged_in:
                        print("🚨 [Emulator] Barge-in Detected! is_speaking=True")
                        is_barged_in = True
            except Exception as e:
                print(f"[Emulator] Disconnected: {e}")

            out_file = f"capture_{session_id}_{int(time.time())}.wav"
            if len(input_audio_frames) > 0:
                print(f"[Emulator] Saving user audio to {out_file}")
                try:
                    with wave.open(out_file, "wb") as wf:
                        wf.setnchannels(1)
                        wf.setsampwidth(2)
                        wf.setframerate(16000)
                        wf.writeframes(input_audio_frames)
                except Exception as e:
                    print(f"Failed to write audio: {e}")

        # Start listening to user audio in the background
        read_task = asyncio.create_task(process_incoming())

        # Simulate initial AI processing delay (0.5s)
        await asyncio.sleep(0.5)

        print(f"[Emulator] Sending welcome TTS to Session {session_id}")
        yield voicebot_pb2.AiResponse(
            type=voicebot_pb2.AiResponse.STT_RESULT, text_content="Hello user from emulator."
        )

        # Send TTS audio chunks (16kHz, 16bit, 1ch -> 640 bytes per 20ms)
        chunk_size = 640
        for i in range(0, len(tts_audio), chunk_size):
            if is_barged_in:
                print("🛑 [Emulator] Halting TTS stream due to barge-in.")
                yield voicebot_pb2.AiResponse(
                    type=voicebot_pb2.AiResponse.END_OF_TURN, clear_buffer=True
                )
                break

            chunk = tts_audio[i : i + chunk_size]
            await asyncio.sleep(0.02)  # Realtime pacing
            yield voicebot_pb2.AiResponse(type=voicebot_pb2.AiResponse.TTS_AUDIO, audio_data=chunk)

        print(f"[Emulator] Finished sending TTS for Session {session_id}")
        yield voicebot_pb2.AiResponse(type=voicebot_pb2.AiResponse.END_OF_TURN)

        # Keep the connection open until client closes to capture all user audio
        await read_task


def create_dummy_wav(filename):
    if not os.path.exists(filename):
        print(f"Creating dummy TTS audio file: {filename}")
        with wave.open(filename, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(16000)
            # Add some audible noise/tones instead of pure silence for testing
            samples = bytearray()
            for i in range(16000 * 5):  # 5 seconds
                val = int(8000 * (i % 2 - 0.5))  # Simple square wave tone
                samples.extend(val.to_bytes(2, byteorder="little", signed=True))
            wf.writeframes(samples)


async def serve():
    create_dummy_wav("sample_tts.wav")

    server = grpc.aio.server()
    voicebot_pb2_grpc.add_VoicebotAiServiceServicer_to_server(
        VoicebotAiEmulator("sample_tts.wav"), server
    )
    bind_port = 0
    bind_addr = ""
    try:
        bind_port = server.add_insecure_port("[::]:50051")
        bind_addr = "[::]:50051"
    except Exception as e:
        print(f"[Emulator] IPv6 bind failed: {e}")

    if bind_port == 0:
        try:
            bind_port = server.add_insecure_port("127.0.0.1:50051")
            bind_addr = "127.0.0.1:50051"
        except Exception as e:
            print(f"[Emulator] IPv4 bind failed: {e}")
    if bind_port == 0:
        raise RuntimeError("Failed to bind emulator gRPC server on 50051 (IPv6/IPv4)")
    print("=======================================")
    print("🚀 STT/TTS Mock Emulator Started!")
    print(f"Listening for Voicebot Gateway on {bind_addr}")
    print("=======================================")
    await server.start()
    await server.wait_for_termination()


if __name__ == "__main__":
    asyncio.run(serve())
