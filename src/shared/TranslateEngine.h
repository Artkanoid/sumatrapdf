/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

enum class TranslateDir {
    EnToFr,
    FrToEn,
};

struct TranslateJob;

TempStr TranslateDirTemp();
bool TranslateInstalled();
TranslateDir TranslateGuessDir(Str text);
TranslateJob* TranslateStart(TranslateDir dir, Str text, void (*onProgress)(int percent), void (*onDone)());
bool TranslateJobDone(TranslateJob* job);
bool TranslateJobTake(TranslateJob* job, Str* textOut);
void TranslateJobCancel(TranslateJob* job);
bool TranslateInstallStart(void (*onProgress)(int percent), void (*onDone)(bool ok));
