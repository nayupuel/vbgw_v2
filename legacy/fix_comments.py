import os
import re

files = [
    "src/engine/VoicebotCall.cpp",
    "src/engine/VoicebotMediaPort.cpp",
    "src/engine/VoicebotAccount.cpp",
    "src/ai/VoicebotAiClient.cpp"
]
for f in files:
    with open(f, 'r') as fp:
        code = fp.read()
    
    # Simple regex to replace std::cout << "..." << std::endl; with spdlog::debug("...");
    # Due to complexity of std::cout concatenation, we just use raw replace where simple
    code = code.replace('std::cout << "[방어 로직] 최대 허용 콜 수를 초과했습니다. 호를 거절(486)합니다." << std::endl;', 'spdlog::warn("[방어 로직] 최대 허용 콜 수를 초과했습니다. 호를 거절(486)합니다.");')
    code = code.replace('std::cout << "\\nIncoming SIP call from Call-ID: " << iprm.callId << std::endl;', 'spdlog::info("Incoming SIP call from Call-ID: {}", iprm.callId);')
    code = code.replace('std::cerr << "Failed to answer call: " << err.info() << std::endl;', 'spdlog::error("Failed to answer call: {}", err.info());')
    
    code = code.replace('std::cout << "🚨 [Barge-In] Flushed Gateway TTS RingBuffer!" << std::endl;', 'spdlog::warn("🚨 [Barge-In] Flushed Gateway TTS RingBuffer!");')
    code = code.replace('std::cerr << "🚨 [Call] Hanging up due to AI Error: " << err << std::endl;', 'spdlog::error("🚨 [Call] Hanging up due to AI Error: {}", err);')
    
    code = code.replace('std::cout << "Call answered." << std::endl;', 'spdlog::info("Call answered.");')
    code = code.replace('std::cerr << "Call disconnection error: " << err.info() << std::endl;', 'spdlog::error("Call disconnection error: {}", err.info());')
    
    if "#include <spdlog/spdlog.h>" not in code:
        code = "#include <spdlog/spdlog.h>\n" + code

    with open(f, 'w') as fp:
        fp.write(code)

print("Replaced std logs with spdlog in core engine files")
