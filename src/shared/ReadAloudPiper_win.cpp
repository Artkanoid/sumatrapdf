/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "base/Http.h"
#include "base/Archive.h"
#include "base/GuessFileType.h"

#include "gui/UIModels.h"

#include "SumatraConfig.h"
#include "Settings.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "DocController.h"
#include "EngineBase.h"
#include "ReadAloud.h"
#include "ReadAloudPiper.h"

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include "SumatraLog.h"

/*
Piper (https://github.com/rhasspy/piper) neural voices for Read Aloud, offline.

Piper is not part of the exe: the user installs it in the app data dir

    piper\piper.exe (+ its dlls and espeak-ng-data)
    piper\voices\<lang>-<name>-<quality>.onnx and .onnx.json

Each chunk of text runs piper.exe on a worker thread and comes back as a WAV
file, which ReadAloud_win.cpp plays like the WinRT synthesis result.
*/

constexpr char kPiperVoicePrefix[] = "piper:";
constexpr int kPiperVoicePrefixLen = sizeof(kPiperVoicePrefix) - 1;
constexpr char kPiperVoiceExt[] = ".onnx";
constexpr int kPiperVoiceExtLen = sizeof(kPiperVoiceExt) - 1;
constexpr DWORD kPiperTimeoutMs = 120 * 1000;
constexpr DWORD kPiperPollMs = 50;
// piper speaks length_scale times slower: 1 / speed
constexpr int kPiperScaleDenom = 100;

enum PiperJobState {
    kPiperRunning,
    kPiperOk,
    kPiperFailed,
};

struct PiperJob {
    AtomicRefCount refs = 2; // the UI thread and the worker
    AtomicInt state = kPiperRunning;
    AtomicBool cancelled = 0;
    AtomicPtr process = nullptr;
    void (*onDone)() = nullptr;

    WCHAR exePath[MAX_PATH]{};
    WCHAR modelPath[MAX_PATH]{};
    WCHAR wavPath[MAX_PATH]{};
    int lengthScale100 = kPiperScaleDenom;
    Str text;
};

static void PiperJobRelease(PiperJob* job) {
    if (AtomicRefCountDec(&job->refs) != 0) {
        return;
    }
    DeleteFileW(job->wavPath);
    str::Free(job->text);
    delete job;
}

TempStr PiperDirTemp() {
    return GetPathInAppDataDirTemp(StrL("piper"));
}

static TempStr PiperVoicesDirTemp() {
    return path::JoinTemp(PiperDirTemp(), StrL("voices"));
}

static TempStr PiperExePathTemp() {
    return path::JoinTemp(PiperDirTemp(), StrL("piper.exe"));
}

bool PiperIsVoiceId(Str id) {
    return str::StartsWith(id, Str(kPiperVoicePrefix));
}

static TempStr PiperModelPathTemp(Str voiceId) {
    if (!PiperIsVoiceId(voiceId)) {
        return {};
    }
    Str base = Str(voiceId.s + kPiperVoicePrefixLen, len(voiceId) - kPiperVoicePrefixLen);
    if (len(base) == 0) {
        return {};
    }
    return path::JoinTemp(PiperVoicesDirTemp(), str::JoinTemp(base, Str(kPiperVoiceExt)));
}

bool PiperVoiceInstalled(Str voiceId) {
    TempStr model = PiperModelPathTemp(voiceId);
    if (len(model) == 0) {
        return false;
    }
    return file::Exists(PiperExePathTemp()) && file::Exists(model);
}

// "fr_FR-siwis-medium" is language "fr-FR", voice "siwis", quality "medium"
static void PiperVoiceFromBase(Str base, TtsVoiceInfo* info) {
    Str lang = base;
    Str rest = {};
    int dash = str::IndexOfChar(base, '-');
    if (dash >= 0) {
        lang = Str(base.s, dash);
        rest = Str(base.s + dash + 1, len(base) - dash - 1);
    }

    TempStr langTag = str::DupTemp(lang);
    for (int i = 0; i < len(langTag); i++) {
        if (langTag.s[i] == '_') {
            langTag.s[i] = '-';
        }
    }

    TempStr name = str::DupTemp(rest);
    for (int i = 0; i < len(name); i++) {
        if (name.s[i] == '-') {
            name.s[i] = ' ';
        }
    }
    if (len(name) == 0) {
        name = str::DupTemp(base);
    }

    info->id = str::Dup(str::JoinTemp(Str(kPiperVoicePrefix), base));
    info->name = str::Dup(fmt("%s (Piper)", name));
    info->lang = str::Dup(langTag);
}

void PiperGetVoices(Vec<TtsVoiceInfo>& voices) {
    if (!file::Exists(PiperExePathTemp())) {
        return;
    }

    TempStr pattern = path::JoinTemp(PiperVoicesDirTemp(), StrL("*.onnx"));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(CWStrTemp(pattern), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        TempStr file = ToUtf8Temp(WStr(fd.cFileName));
        Str base = Str(file.s, len(file) - kPiperVoiceExtLen);
        TtsVoiceInfo info{};
        PiperVoiceFromBase(base, &info);
        VecAppend(voices, info);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void PiperKill(PiperJob* job) {
    HANDLE proc = (HANDLE)job->process;
    if (proc) {
        TerminateProcess(proc, 1);
    }
}

// runs piper.exe with the text on its stdin; the result is the WAV file
static bool PiperRun(PiperJob* job) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inRead = nullptr;
    HANDLE inWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    WCHAR cmd[4 * MAX_PATH]{};
    swprintf_s(cmd, L"\"%s\" --model \"%s\" --output_file \"%s\" --length_scale %d.%02d --quiet", job->exePath,
               job->modelPath, job->wavPath, job->lengthScale100 / kPiperScaleDenom,
               job->lengthScale100 % kPiperScaleDenom);

    WCHAR dir[MAX_PATH]{};
    wcscpy_s(dir, job->exePath);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
    CloseHandle(inRead);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!started) {
        CloseHandle(inWrite);
        logf("tts: piper: CreateProcess failed: %d\n", (int)GetLastError());
        return false;
    }

    // either side may win the race with PiperJobCancel
    AtomicPtrExchange(&job->process, pi.hProcess);
    if (AtomicBoolGet(&job->cancelled)) {
        PiperKill(job);
    }

    DWORD written = 0;
    WriteFile(inWrite, job->text.s, (DWORD)len(job->text), &written, nullptr);
    WriteFile(inWrite, "\n", 1, &written, nullptr);
    CloseHandle(inWrite);

    DWORD waited = 0;
    bool exited = false;
    while (!AtomicBoolGet(&job->cancelled) && waited < kPiperTimeoutMs) {
        if (WaitForSingleObject(pi.hProcess, kPiperPollMs) == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        waited += kPiperPollMs;
    }
    if (!exited) {
        PiperKill(job);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    AtomicPtrExchange(&job->process, nullptr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!exited || exitCode != 0) {
        if (!AtomicBoolGet(&job->cancelled)) {
            logf("tts: piper: failed exited=%d code=%d\n", (int)exited, (int)exitCode);
        }
        return false;
    }
    return true;
}

static DWORD WINAPI PiperThread(void* arg) {
    PiperJob* job = (PiperJob*)arg;
    bool ok = PiperRun(job);
    bool cancelled = AtomicBoolGet(&job->cancelled);
    AtomicIntSet(&job->state, ok ? kPiperOk : kPiperFailed);
    if (!cancelled && job->onDone) {
        job->onDone();
    }
    PiperJobRelease(job);
    return 0;
}

PiperJob* PiperStart(Str voiceId, Str textUtf8, float speed, void (*onDone)()) {
    TempStr model = PiperModelPathTemp(voiceId);
    if (len(model) == 0 || len(textUtf8) == 0 || !PiperVoiceInstalled(voiceId)) {
        logf("tts: piper: voice '%s' not installed in '%s'\n", voiceId, PiperDirTemp());
        return nullptr;
    }
    TempStr exe = PiperExePathTemp();
    TempStr wav = GetTempFilePathTemp(StrL("piper"));
    if (len(wav) == 0) {
        return nullptr;
    }

    auto* job = new PiperJob();
    job->onDone = onDone;
    wcscpy_s(job->exePath, CWStrTemp(exe));
    wcscpy_s(job->modelPath, CWStrTemp(model));
    wcscpy_s(job->wavPath, CWStrTemp(wav));
    job->lengthScale100 = (int)((float)kPiperScaleDenom / (speed > 0 ? speed : 1.0f) + 0.5f);
    job->text = str::Dup(textUtf8);

    HANDLE thread = CreateThread(nullptr, 0, PiperThread, job, 0, nullptr);
    if (!thread) {
        PiperJobRelease(job);
        PiperJobRelease(job);
        return nullptr;
    }
    CloseHandle(thread);
    return job;
}

bool PiperJobDone(PiperJob* job) {
    return AtomicIntGet(&job->state) != kPiperRunning;
}

// hands the WAV (malloc'd) to the caller and drops the UI's reference
bool PiperJobTakeWav(PiperJob* job, u8** dataOut, DWORD* sizeOut) {
    bool ok = false;
    if (AtomicIntGet(&job->state) == kPiperOk) {
        Str data = file::ReadFile(ToUtf8Temp(WStr(job->wavPath)));
        if (len(data) > 0) {
            *dataOut = (u8*)data.s;
            *sizeOut = (DWORD)len(data);
            ok = true;
        }
    }
    PiperJobRelease(job);
    return ok;
}

void PiperJobCancel(PiperJob* job) {
    AtomicBoolSet(&job->cancelled, true);
    PiperKill(job);
    PiperJobRelease(job);
}

//--- installing Piper and a French voice

// pinned: the install only accepts exactly these files
constexpr char kPiperEngineUrl[] =
    "https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip";
constexpr char kPiperEngineSha256[] = "f3c58906402b24f3a96d92145f58acba6d86c9b5db896d207f78dc80811efcea";
constexpr i64 kPiperEngineBytes = 22477236;
constexpr char kPiperEngineZipPrefix[] = "piper/";
constexpr int kPiperEngineZipPrefixLen = sizeof(kPiperEngineZipPrefix) - 1;

constexpr char kPiperDefaultVoice[] = "fr_FR-siwis-medium";
constexpr char kPiperVoiceUrl[] =
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium/fr_FR-siwis-medium.onnx";
constexpr char kPiperVoiceSha256[] = "641d1ab097da2b81128c076810edb052b385decc8be3381814802a64a73baf99";
constexpr i64 kPiperVoiceBytes = 63201294;
constexpr char kPiperVoiceJsonUrl[] =
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium/fr_FR-siwis-medium.onnx.json";
constexpr char kPiperVoiceJsonSha256[] = "39479916c2db192b5ac9764daddd0c744d83e023ad890c6976c0633ae4df8959";

// the progress bar, in percent: engine download, then unzip, then the voice
constexpr int kPercentEngineDownload = 26;
constexpr int kPercentEngineExtract = 30;
constexpr int kPercentVoiceDone = 100;

constexpr char kPartExt[] = ".part";

TempStr PiperDefaultVoiceId() {
    return str::JoinTemp(Str(kPiperVoicePrefix), Str(kPiperDefaultVoice));
}

bool PiperEngineInstalled() {
    return file::Exists(PiperExePathTemp());
}

static bool Sha256HexMatches(Str data, const char* expectedHex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    u8 digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, (PUCHAR)data.s, (ULONG)len(data), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) {
        return false;
    }
    char hex[sizeof(digest) * 2 + 1]{};
    for (size_t i = 0; i < sizeof(digest); i++) {
        sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    }
    return str::EqI(Str(hex), Str(expectedHex));
}

struct PiperInstall {
    void (*onProgress)(int percent) = nullptr;
    void (*onDone)(bool ok) = nullptr;
    int base = 0;  // percent done before the current download
    int share = 0; // percent the current download adds
    i64 expectedBytes = 1;
};

static PiperInstall* gInstall = nullptr;

static void InstallOnDownload(PiperInstall* in, HttpProgress* p) {
    i64 done = p->nDownloaded < in->expectedBytes ? p->nDownloaded : in->expectedBytes;
    in->onProgress(in->base + (int)(done * in->share / in->expectedBytes));
}

// downloads to dest.part, checks the SHA-256, then moves it to dest
static bool InstallDownload(PiperInstall* in, const char* url, Str dest, const char* sha256, i64 bytes, int base,
                            int share) {
    in->base = base;
    in->share = share;
    in->expectedBytes = bytes;

    TempStr part = str::JoinTemp(dest, Str(kPartExt));
    dir::CreateForFile(dest);
    auto cb = MkFunc1(InstallOnDownload, in);
    if (!HttpGetToFile(Str(url), part, cb)) {
        logf("tts: piper: download failed '%s'\n", Str(url));
        file::Delete(part);
        return false;
    }
    Str data = file::ReadFile(part);
    bool ok = len(data) > 0 && Sha256HexMatches(data, sha256);
    str::Free(data);
    if (!ok) {
        logf("tts: piper: hash mismatch '%s'\n", Str(url));
        file::Delete(part);
        return false;
    }
    return MoveFileExW(CWStrTemp(part), CWStrTemp(dest), MOVEFILE_REPLACE_EXISTING) != 0;
}

static bool NameIsSafe(Str name) {
    if (len(name) == 0 || str::IndexOf(name, StrL("..")) >= 0 || str::IndexOfChar(name, ':') >= 0) {
        return false;
    }
    return name.s[0] != '/' && name.s[0] != '\\';
}

// piper_windows_amd64.zip holds everything under "piper/"
static bool InstallExtractEngine(Str zipPath) {
    Archive archive;
    if (!archive.Open(zipPath, true, FileType::Unknown, ArchiveExtractProgressCb())) {
        return false;
    }
    TempStr dir = PiperDirTemp();
    bool gotExe = false;
    for (Archive::FileInfo* fi : archive.GetFileInfos()) {
        if (fi->isDir || !str::StartsWith(fi->name, Str(kPiperEngineZipPrefix))) {
            continue;
        }
        Str rel = Str(fi->name.s + kPiperEngineZipPrefixLen, len(fi->name) - kPiperEngineZipPrefixLen);
        if (!NameIsSafe(rel)) {
            logf("tts: piper: unsafe name in zip '%s'\n", fi->name);
            return false;
        }
        // nothing to write for an empty entry
        if (fi->fileSizeUncompressed == 0) {
            continue;
        }
        Archive::FileInfo* data = archive.GetFileDataById(fi->fileId);
        if (!data || !data->data) {
            logf("tts: piper: cannot extract '%s'\n", fi->name);
            return false;
        }
        TempStr relWin = str::DupTemp(rel);
        for (int i = 0; i < len(relWin); i++) {
            if (relWin.s[i] == '/') {
                relWin.s[i] = '\\';
            }
        }
        TempStr dest = path::JoinTemp(dir, relWin);
        dir::CreateForFile(dest);
        if (!file::WriteFile(dest, Str(data->data, data->fileSizeUncompressed))) {
            logf("tts: piper: cannot write '%s'\n", dest);
            return false;
        }
        gotExe = gotExe || str::EqI(rel, StrL("piper.exe"));
    }
    return gotExe;
}

static void InstallRun(PiperInstall* in) {
    TempStr dir = PiperDirTemp();
    TempStr zipPath = path::JoinTemp(dir, StrL("piper_windows_amd64.zip"));
    bool ok = true;

    if (!PiperEngineInstalled()) {
        ok = InstallDownload(in, kPiperEngineUrl, zipPath, kPiperEngineSha256, kPiperEngineBytes, 0,
                             kPercentEngineDownload);
        ok = ok && InstallExtractEngine(zipPath);
        file::Delete(zipPath);
        logf("tts: piper: engine step ok=%d\n", (int)ok);
        in->onProgress(kPercentEngineExtract);
    }

    if (ok && !PiperVoiceInstalled(PiperDefaultVoiceId())) {
        TempStr voiceDir = path::JoinTemp(dir, StrL("voices"));
        TempStr onnx = path::JoinTemp(voiceDir, str::JoinTemp(Str(kPiperDefaultVoice), Str(kPiperVoiceExt)));
        TempStr json = str::JoinTemp(onnx, StrL(".json"));
        ok = InstallDownload(in, kPiperVoiceJsonUrl, json, kPiperVoiceJsonSha256, 1, kPercentEngineExtract, 0);
        ok = ok && InstallDownload(in, kPiperVoiceUrl, onnx, kPiperVoiceSha256, kPiperVoiceBytes, kPercentEngineExtract,
                                   kPercentVoiceDone - kPercentEngineExtract);
    }
    logf("tts: piper: install done ok=%d\n", (int)ok);
    in->onDone(ok);
}

static DWORD WINAPI InstallThread(void* arg) {
    auto* in = (PiperInstall*)arg;
    InstallRun(in);
    gInstall = nullptr;
    delete in;
    ResetTempArena();
    return 0;
}

// one install at a time; the callbacks run on the worker thread
bool PiperInstallStart(void (*onProgress)(int percent), void (*onDone)(bool ok)) {
    if (gInstall) {
        return false;
    }
    auto* in = new PiperInstall();
    in->onProgress = onProgress;
    in->onDone = onDone;
    gInstall = in;
    HANDLE thread = CreateThread(nullptr, 0, InstallThread, in, 0, nullptr);
    if (!thread) {
        gInstall = nullptr;
        delete in;
        return false;
    }
    CloseHandle(thread);
    return true;
}
