"""Speech in, speech out for the robot.

  transcribe(pcm)  -> text        (Groq Whisper, free tier, via the OpenAI-compatible API)
  speak(text)      -> audio_id    (edge-tts, free, then decoded to 16 kHz mono PCM)

Audio format between robot and server: raw 16-bit little-endian mono PCM, 16 kHz.
"""
import io
import os
import uuid
import wave
from collections import OrderedDict

import assistant

SAMPLE_RATE = 16000
STT_MODEL = os.getenv("STT_MODEL", "whisper-large-v3-turbo")
STT_LANGUAGE = os.getenv("STT_LANGUAGE", "en")
TTS_VOICE = os.getenv("TTS_VOICE", "en-US-AriaNeural")

# Words Whisper sometimes invents from background noise.
_NOISE_WORDS = {"", "you", "uh", "um", "hmm"}

_audio = OrderedDict()  # audio_id -> PCM bytes (the robot downloads it once)
_phrase_ids = {}        # text -> audio_id (short fixed phrases are only synthesized once)


def _store(pcm):
    audio_id = uuid.uuid4().hex[:12]
    _audio[audio_id] = pcm
    while len(_audio) > 30:
        _audio.popitem(last=False)
    return audio_id


def get_audio(audio_id):
    return _audio.get(audio_id)


def pcm_to_wav(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def transcribe(pcm):
    """Blocking call: run it in a thread pool from async code."""
    result = assistant.client().audio.transcriptions.create(
        model=STT_MODEL,
        file=("speech.wav", pcm_to_wav(pcm), "audio/wav"),
        language=STT_LANGUAGE,
        prompt="Personal schedule assistant. Meetings, alarms, plans, cancel, move, reschedule.",
        temperature=0,
    )
    text = (result.text or "").strip()
    if text.strip(" .!?,").lower() in _NOISE_WORDS:
        return ""
    return text


async def _synthesize(text):
    import edge_tts   # pip install edge-tts
    import miniaudio  # pip install miniaudio

    mp3 = bytearray()
    async for chunk in edge_tts.Communicate(text, TTS_VOICE).stream():
        if chunk["type"] == "audio":
            mp3 += chunk["data"]
    if not mp3:
        raise RuntimeError("edge-tts returned no audio")
    decoded = miniaudio.decode(
        bytes(mp3),
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=SAMPLE_RATE,
    )
    return decoded.samples.tobytes()


async def speak(text):
    """Returns an audio_id the robot can download, or None if TTS failed
    (the robot then shows the text on the OLED instead)."""
    if not text:
        return None
    cached = _phrase_ids.get(text)
    if cached and cached in _audio:
        return cached
    try:
        audio_id = _store(await _synthesize(text))
    except Exception as exc:
        print(f"TTS failed: {exc!r}  (try: pip install -U edge-tts)")
        return None
    if len(text) < 120:
        _phrase_ids[text] = audio_id
    return audio_id
