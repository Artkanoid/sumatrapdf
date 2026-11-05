/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct PiperJob;

TempStr PiperDirTemp();
bool PiperIsVoiceId(Str id);
bool PiperVoiceInstalled(Str id);
void PiperGetVoices(Vec<TtsVoiceInfo>& voices);
PiperJob* PiperStart(Str voiceId, Str textUtf8, float speed, void (*onDone)());
bool PiperJobDone(PiperJob* job);
bool PiperJobTakeWav(PiperJob* job, u8** dataOut, DWORD* sizeOut);
void PiperJobCancel(PiperJob* job);

TempStr PiperDefaultVoiceId();
bool PiperEngineInstalled();
bool PiperInstallStart(void (*onProgress)(int percent), void (*onDone)(bool ok));
