from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class AudioChunk(_message.Message):
    __slots__ = ("session_id", "audio_data", "is_speaking")
    SESSION_ID_FIELD_NUMBER: _ClassVar[int]
    AUDIO_DATA_FIELD_NUMBER: _ClassVar[int]
    IS_SPEAKING_FIELD_NUMBER: _ClassVar[int]
    session_id: str
    audio_data: bytes
    is_speaking: bool
    def __init__(self, session_id: _Optional[str] = ..., audio_data: _Optional[bytes] = ..., is_speaking: bool = ...) -> None: ...

class AiResponse(_message.Message):
    __slots__ = ("type", "text_content", "audio_data", "clear_buffer")
    class ResponseType(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
        __slots__ = ()
        STT_RESULT: _ClassVar[AiResponse.ResponseType]
        TTS_AUDIO: _ClassVar[AiResponse.ResponseType]
        END_OF_TURN: _ClassVar[AiResponse.ResponseType]
    STT_RESULT: AiResponse.ResponseType
    TTS_AUDIO: AiResponse.ResponseType
    END_OF_TURN: AiResponse.ResponseType
    TYPE_FIELD_NUMBER: _ClassVar[int]
    TEXT_CONTENT_FIELD_NUMBER: _ClassVar[int]
    AUDIO_DATA_FIELD_NUMBER: _ClassVar[int]
    CLEAR_BUFFER_FIELD_NUMBER: _ClassVar[int]
    type: AiResponse.ResponseType
    text_content: str
    audio_data: bytes
    clear_buffer: bool
    def __init__(self, type: _Optional[_Union[AiResponse.ResponseType, str]] = ..., text_content: _Optional[str] = ..., audio_data: _Optional[bytes] = ..., clear_buffer: bool = ...) -> None: ...
