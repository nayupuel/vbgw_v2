# Voicebot Gateway (VBGW) 🚀

환영합니다! **Voicebot Gateway(VBGW)**는 전화를 걸고 받을 수 있는 AI 콜봇을 만들기 위한 **핵심 통화 게이트웨이 시스템**입니다. 
사용자의 목소리를 듣고(STT), 똑똑하게 대답하고(TTS), 사용자가 말을 끊으면 즉시 듣기 모드로 전환(Barge-in)하는 강력한 기능들을 제공합니다.

이 문서는 **"프로그래밍 배경지식이 적은 초보자나 인프라 엔지니어도 처음부터 끝까지 따라 할 수 있도록"** 
가장 친절하고 상세하게 환경 구성부터 테스트까지 안내합니다. 차근차근 따라와 주세요!

---

## 🎯 1. 프로그램 개요 (무엇을 하는 프로그램인가요?)

일반적인 웹이나 앱과 달리 전화망(SIP/PBX) 모델과 AI(gRPC) 모델은 서로 통신하는 방식이 완전히 다릅니다.
VBGW는 이 둘 사이에서 완벽한 통역사 역할을 합니다.

1. **전화망(SIP) 역할**: 일반 전화기(Softphone)나 기업용 통신 장비(PBX)로부터 걸려오는 전화를 받습니다.
2. **미디어 변환 (RTP ↔ PCM)**: 전화기에서 오는 8kHz 오디오를 AI가 이해할 수 있는 16kHz 고음질 오디오로 실시간 변환합니다.
3. **AI 플러그 (gRPC)**: 변환된 오디오를 AI 엔진(STT/TTS)에 초고속으로 스트리밍하여 실시간 대화를 가능하게 합니다.
4. **로컬 Edge VAD**: 게이트웨이 자체가 마이크의 소리를 분석해 "사용자가 말을 하고 있는지(음성 감지)"를 스스로 판단합니다.

---

## 🛠️ 2. Step 1: 개발 환경 구성 (Prerequisites)

이 프로젝트는 **macOS (Apple Silicon M1/M2/M3 권장)** 환경에 최적화되어 있습니다.
터미널(Terminal) 앱을 열고 아래 순서대로 명령어를 복사해서 붙여넣어 주세요.

### 2.1 Homebrew 설치 (패키지 관리자)
Mac에서 개발 도구들을 쉽게 설치하기 위한 프로그램입니다. (이미 설치되어 있다면 넘어갑니다.)
```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### 2.2 필수 라이브러리 설치
C++ 게이트웨이를 빌드하기 위해 필요한 부품(라이브러리)들을 설치합니다. 한 줄씩 복사해서 실행하세요.
```bash
brew update
brew install cmake pjproject grpc protobuf openssl spdlog boost onnxruntime
```
*💡 설치 중 에러가 발생하면 무시하지 말고 해당 에러 메시지를 검색하거나 `docs/troubleshooting.md`를 참고하세요.*

---

## 🏗️ 3. Step 2: VBGW 게이트웨이 빌드 및 설치

통신 서버(게이트웨이) 프로그램을 뚝딱뚝딱 조립(빌드)하는 과정입니다.

1. 터미널을 열고 VBGW 프로젝트 폴더(이 파일이 있는 곳)로 이동합니다.
2. 아래 명령어를 순서대로 실행합니다.

```bash
# 1. 빌드용 폴더를 만들고 그 안으로 들어갑니다.
mkdir -p build && cd build

# 2. CMake를 통해 빌드 설계도를 작성합니다.
cmake ..

# 3. 작성된 설계도를 바탕으로 실제 프로그램을 컴파일합니다. (컴퓨터 코어 수만큼 빠르게 빌드)
make -j$(sysctl -n hw.ncpu)
```

✅ **성공 확인**: 빌드가 100% 완료되면 `build/` 폴더 안에 `vbgw` 라는 초록색 실행 파일이 생성됩니다.

---

## 🤖 4. Step 3: 가상의 AI 엔진 (에뮬레이터) 설치

우리에게는 아직 진짜 AI(STT/TTS) 서버가 없습니다. 따라서 **게이트웨이랑 통신하며 가짜로 대답해주는 AI 모의 서버(Python 에뮬레이터)**를 세팅해야 합니다.

터미널을 **새로 하나 더 열고(혹은 새 탭)** 프로젝트 루트 폴더에서 다음을 진행합니다.

```bash
# 1. 에뮬레이터 폴더로 이동
cd src/emulator

# 2. 파이썬 격리 환경(가상환경) 생성 (컴퓨터 환경이 꼬이지 않게 보호)
python3 -m venv venv

# 3. 가상환경 접속 (프롬프트 왼쪽에 (venv)가 생김)
source venv/bin/activate

# 4. 에뮬레이터 구동에 필요한 파이썬 패키지 설치
pip install -r requirements.txt
```

---

## ⚙️ 5. Step 4: 환경 변수(설정 값) 세팅

VBGW 게이트웨이는 전화를 어디로 받을지, 로그는 얼마나 자세히 찍을지 등을 설정 파일(`.env`)에서 관리합니다.

1. 프로젝트 최상위 폴더(루트)로 돌아옵니다.
2. 친절하게 준비된 예제 설정 파일을 실제 설정 파일로 복사합니다.

```bash
cp config/.env.example .env
```

3. (선택) `.env` 파일을 텍스트 에디터로 열어봅니다. 당장 건드릴 것은 없지만, 궁금하시면 설정값을 바꿀 수 있습니다.
4. 터미널에 설정값을 반영합니다. (게이트웨이를 실행할 터미널 창에서 매번 입력해 주어야 합니다.)

```bash
source .env
```

---

## 🎧 6. Step 5: 본격적인 테스트 진행! (Testing Guide)

자, 이제 모든 준비가 끝났습니다. 화면에 두 개의 터미널 창(또는 탭)을 띄워 놓고 따라 하세요.

### 터미널 [A]: AI 에뮬레이터 켜기
먼저 AI 서버부터 깨워야 합니다.
```bash
cd src/emulator
source venv/bin/activate

# Mock Server 실행! (대기 모드로 들어갑니다)
python emulator.py
```
*(예상 화면: "AI Mock Server listening on port 50051...")*


### 터미널 [B]: VBGW 게이트웨이 켜기
AI가 준비되었으니 게이트웨이를 켭니다.
```bash
# 프로젝트 루트 폴더인지 확인 후
source .env
./build/vbgw
```
*(예상 화면: "[VBGW] Local Mode Enabled... Direct IP calls: sip:voicebot@127.0.0.1")*

---

### 터미널 [C] (내 노트북): SIP 통화 앱(Softphone)으로 전화 걸기!
전화를 걸 스마트폰 역할의 앱이 PC에 하나 필요합니다. 가장 많이 쓰이는 무료 프로그램 **Linphone**을 추천합니다.

1. [Linphone 공식 홈페이지](https://new.linphone.org/software)에서 데스크톱 버전을 다운로드 및 설치합니다.
2. 프로그램을 실행하고 "계정 생성"이나 "로그인"은 모두 **건너뛰기(Skip)** 합니다. (회원가입 필요 없음!)
3. 메인 화면 맨 위 검색/다이얼 창에 다음 주소를 입력합니다:
   👉 **`sip:voicebot@127.0.0.1:5060`**
4. 전화 아이콘📞 을 눌러 통화를 시작해 봅니다.

### 🎉 테스트 시크립트 (이렇게 대화해보세요)

| 단계 | 나의 행동 | 시스템의 반응 |
| :--- | :--- | :--- |
| **연결 성공** | 전화를 받음 | 에뮬레이터(터미널[A])에 `New SIP Call connected` 로그가 찍힘. |
| **인식 (VAD)** | "안녕하세요?" 하고 **말함** | 게이트웨이(터미널[B])에 `[VAD] Speaking started` 로그가 찍힘. |
| **답변 (TTS)** | 말을 멈추고 **기다림** | 에뮬레이터가 삐- 소리나 샘플 답변을 보냄, **내 헤드셋 스피커로 소리가 나옴!** |
| **말끊기 (Barge-in)** | 스피커에서 답변 소리가 나오는 **도중에 내가 다시 시끄럽게 떠듦** | 💡 AI의 말소리가 **즉각 뚝 끊기며**, 다시 나의 말을 듣기 모드로 전환됨! |
| **종료** | 빨간색 종료 버튼을 누름 | 깔끔하게 세션이 종료(Disconnected) 됨. |

---

## 📚 7. 잘 안 되나요? (Troubleshooting)

만약 에러가 발생하거나 전화 연결이 안 된다면 걱정하지 마세요. 자주 발생하는 문제의 해결법을 모아두었습니다!

- **문제가 생겼을 때**: 👉 [docs/troubleshooting.md](docs/troubleshooting.md) 를 클릭해서 읽어보세요. 
- 시스템 구조가 궁금할 때: 👉 [docs/architecture.md](docs/architecture.md)
- API 연동 방법이 궁금할 때: 👉 [docs/api_spec.md](docs/api_spec.md)

---
*© 2026 Voicebot Gateway Team. 개발자들을 응원합니다!*
