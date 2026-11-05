/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "base/Archive.h"
#include "base/GuessFileType.h"

#include "AppTools.h"
#include "TranslateEngine.h"
#include "VerifiedDownload.h"

#include "SumatraLog.h"

/*
Offline translation. src/tools/translate/translate.cpp (built by
cmd/build-translate.ts) runs a CTranslate2 Opus-MT model; this runs it.

Installed in the app data dir, nothing in the exe:

    translate\sumatrapdf-translate.exe
    translate\ctranslate2.dll, libiomp5md.dll   (from the CTranslate2 wheel)
    translate\models\en-fr\, translate\models\fr-en\

The text goes to the helper on stdin in blocks, each followed by a form feed;
the helper answers each block with the translated lines and a form feed.
*/

constexpr char kHelperExe[] = "sumatrapdf-translate.exe";
constexpr char kCt2Dll[] = "ctranslate2.dll";
constexpr char kOmpDll[] = "libiomp5md.dll";
constexpr char kModelFile[] = "model.bin";
constexpr char kModelEnFr[] = "en-fr";
constexpr char kModelFrEn[] = "fr-en";

constexpr char kFlushChar = '\f';
constexpr int kBlockChars = 4000;
constexpr int kReadChunk = 64 * 1024;
constexpr int kPipeBuffer = 1024 * 1024;
constexpr DWORD kExitWaitMs = 10 * 1000;
constexpr int kMaxThreads = 8;
constexpr int kGuessSampleChars = 6000;

enum TranslateState {
    kTrRunning,
    kTrOk,
    kTrFailed,
};

struct TranslateJob {
    AtomicRefCount refs = 2; // the UI thread and the worker
    AtomicInt state = kTrRunning;
    AtomicBool cancelled = 0;
    AtomicPtr process = nullptr;
    void (*onProgress)(int percent) = nullptr;
    void (*onDone)() = nullptr;

    WCHAR exePath[MAX_PATH]{};
    WCHAR modelPath[MAX_PATH]{};
    int threads = 1;
    Str text;
    Str result;
};

static void JobRelease(TranslateJob* job) {
    if (AtomicRefCountDec(&job->refs) != 0) {
        return;
    }
    str::Free(job->text);
    str::Free(job->result);
    delete job;
}

TempStr TranslateDirTemp() {
    return GetPathInAppDataDirTemp(StrL("translate"));
}

static TempStr InDir(Str name) {
    return path::JoinTemp(TranslateDirTemp(), name);
}

static TempStr ModelDirTemp(TranslateDir dir) {
    Str name = dir == TranslateDir::EnToFr ? Str(kModelEnFr) : Str(kModelFrEn);
    return path::JoinTemp(InDir(StrL("models")), name);
}

static bool ModelInstalled(TranslateDir dir) {
    return file::Exists(path::JoinTemp(ModelDirTemp(dir), Str(kModelFile)));
}

bool TranslateInstalled() {
    return file::Exists(InDir(Str(kHelperExe))) && file::Exists(InDir(Str(kCt2Dll))) &&
           file::Exists(InDir(Str(kOmpDll))) && ModelInstalled(TranslateDir::EnToFr) &&
           ModelInstalled(TranslateDir::FrToEn);
}

static int CountWord(Str lower, Str word) {
    int n = 0;
    int from = 0;
    for (;;) {
        int idx = str::IndexOf(Str(lower.s + from, len(lower) - from), word);
        if (idx < 0) {
            return n;
        }
        n++;
        from += idx + len(word);
    }
}

// English or French: the more of the common words of one, the one it is
TranslateDir TranslateGuessDir(Str text) {
    int n = len(text) < kGuessSampleChars ? len(text) : kGuessSampleChars;
    TempStr lower = str::DupTemp(Str(text.s, n));
    for (int i = 0; i < len(lower); i++) {
        char c = lower.s[i];
        lower.s[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    const char* en[] = {" the ", " and ", " of ", " to ", " is ", " in ", " that ", " with "};
    const char* fr[] = {" le ", " la ", " les ", " de ", " des ", " et ", " est ", " que "};
    int nEn = 0;
    int nFr = 0;
    for (const char* w : en) {
        nEn += CountWord(lower, Str(w));
    }
    for (const char* w : fr) {
        nFr += CountWord(lower, Str(w));
    }
    return nFr > nEn ? TranslateDir::FrToEn : TranslateDir::EnToFr;
}

static void Kill(TranslateJob* job) {
    HANDLE proc = (HANDLE)job->process;
    if (proc) {
        TerminateProcess(proc, 1);
    }
}

// reads from the helper until the form feed line that ends its answer
static bool ReadAnswer(TranslateJob* job, HANDLE readPipe, str::Builder& answer) {
    str::Builder buf;
    char chunk[kReadChunk];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(readPipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) {
            return false;
        }
        buf.Append(Str(chunk, (int)n));
        Str all = ToStr(buf);
        int l = len(all);
        if (l >= 2 && all.s[l - 2] == kFlushChar && all.s[l - 1] == '\n') {
            answer.Append(Str(all.s, l - 2));
            return true;
        }
        if (AtomicBoolGet(&job->cancelled)) {
            return false;
        }
    }
}

// the end of the block that starts at pos: whole lines, about kBlockChars
static int BlockEnd(Str text, int pos) {
    int end = pos;
    int n = len(text);
    while (end < n && end - pos < kBlockChars) {
        int nl = str::IndexOfChar(Str(text.s + end, n - end), '\n');
        end = nl < 0 ? n : end + nl + 1;
    }
    return end;
}

static bool RunHelper(TranslateJob* job) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inRead = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, kPipeBuffer) || !CreatePipe(&outRead, &outWrite, &sa, kPipeBuffer)) {
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    WCHAR cmd[3 * MAX_PATH]{};
    swprintf_s(cmd, L"\"%s\" \"%s\" %d", job->exePath, job->modelPath, job->threads);
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
    si.hStdOutput = outWrite;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!started) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        logf("translate: CreateProcess failed: %d\n", (int)GetLastError());
        return false;
    }
    AtomicPtrExchange(&job->process, pi.hProcess);
    if (AtomicBoolGet(&job->cancelled)) {
        Kill(job);
    }

    str::Builder answer;
    bool ok = true;
    int total = len(job->text);
    int pos = 0;
    while (ok && pos < total && !AtomicBoolGet(&job->cancelled)) {
        int end = BlockEnd(job->text, pos);
        DWORD written = 0;
        const char flush[2] = {kFlushChar, '\n'};
        ok = WriteFile(inWrite, job->text.s + pos, (DWORD)(end - pos), &written, nullptr) &&
             WriteFile(inWrite, flush, sizeof(flush), &written, nullptr) && ReadAnswer(job, outRead, answer);
        pos = end;
        if (ok && job->onProgress) {
            job->onProgress((int)((i64)pos * 100 / total));
        }
    }
    CloseHandle(inWrite);
    CloseHandle(outRead);

    if (WaitForSingleObject(pi.hProcess, kExitWaitMs) != WAIT_OBJECT_0) {
        Kill(job);
    }
    AtomicPtrExchange(&job->process, nullptr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    ok = ok && !AtomicBoolGet(&job->cancelled);
    if (ok) {
        job->result = str::Dup(ToStr(answer));
    } else if (!AtomicBoolGet(&job->cancelled)) {
        logf("translate: the helper failed\n");
    }
    return ok;
}

static DWORD WINAPI TranslateThread(void* arg) {
    auto* job = (TranslateJob*)arg;
    bool ok = RunHelper(job);
    bool cancelled = AtomicBoolGet(&job->cancelled);
    AtomicIntSet(&job->state, ok ? kTrOk : kTrFailed);
    if (!cancelled && job->onDone) {
        job->onDone();
    }
    JobRelease(job);
    return 0;
}

TranslateJob* TranslateStart(TranslateDir dir, Str text, void (*onProgress)(int percent), void (*onDone)()) {
    if (len(text) == 0 || !TranslateInstalled()) {
        return nullptr;
    }
    auto* job = new TranslateJob();
    job->onProgress = onProgress;
    job->onDone = onDone;
    wcscpy_s(job->exePath, CWStrTemp(InDir(Str(kHelperExe))));
    wcscpy_s(job->modelPath, CWStrTemp(ModelDirTemp(dir)));
    int cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    job->threads = ClampI(cores / 2, 1, kMaxThreads);
    job->text = str::Dup(text);

    HANDLE thread = CreateThread(nullptr, 0, TranslateThread, job, 0, nullptr);
    if (!thread) {
        JobRelease(job);
        JobRelease(job);
        return nullptr;
    }
    CloseHandle(thread);
    return job;
}

bool TranslateJobDone(TranslateJob* job) {
    return AtomicIntGet(&job->state) != kTrRunning;
}

// hands the translation (a copy the caller frees) over and drops the UI's reference
bool TranslateJobTake(TranslateJob* job, Str* textOut) {
    bool ok = AtomicIntGet(&job->state) == kTrOk && len(job->result) > 0;
    if (ok) {
        *textOut = str::Dup(job->result);
    }
    JobRelease(job);
    return ok;
}

void TranslateJobCancel(TranslateJob* job) {
    AtomicBoolSet(&job->cancelled, true);
    Kill(job);
    JobRelease(job);
}

//--- install

// pinned: the install only accepts exactly these files
struct InstallItem {
    const char* url;
    const char* sha256;
    i64 bytes;
    const char* dest; // under translate\, backslashes
};

constexpr char kHelperUrl[] =
    "https://github.com/Artkanoid/sumatrapdf/releases/download/translate-1/sumatrapdf-translate.exe";
constexpr char kWheelUrl[] =
    "https://files.pythonhosted.org/packages/20/b9/3ae145ed28a3d11ddfe5e3e98c8cd56ffef3c914e61b65790364e9e602c0/"
    "ctranslate2-4.8.5-cp313-cp313-win_amd64.whl";
constexpr char kWheelDest[] = "ctranslate2.whl";
constexpr char kWheelDllPrefix[] = "ctranslate2/";
constexpr int kWheelDllPrefixLen = sizeof(kWheelDllPrefix) - 1;

// Opus-MT (Helsinki-NLP, CC BY 4.0) converted for CTranslate2, int8
constexpr char kEnFrBase[] =
    "https://huggingface.co/craftwise/ct2-opus-mt-en-fr-int8/resolve/1bd14ddf7b73d0d5ef2dbe4982e2cfdadec9c0f2/";
constexpr char kFrEnBase[] =
    "https://huggingface.co/craftwise/ct2-opus-mt-fr-en-int8/resolve/a85209987bcca46b0c206c619e435145da62d193/";

struct ModelFile {
    const char* name;
    i64 bytes;
    const char* sha256EnFr;
    const char* sha256FrEn;
};

constexpr ModelFile kModelFiles[] = {
    {"config.json", 223, "8f6496adfc930cbfecbe8281112197705c488fab47d34b4829b06d7f478909af",
     "8f6496adfc930cbfecbe8281112197705c488fab47d34b4829b06d7f478909af"},
    {"model.bin", 76714434, "3714b6de0ccd7067adccd76874d2b15aa60918738caaec5692b8af4bb00de597",
     "10a8db66d56d69042d4a902b86950915c547173147fd9949a972c3b2d97c2ddc"},
    {"shared_vocabulary.json", 1052697, "a157eb51a42817dd1ae4bc46cecf66e591ab32641df936da292bd09f2728631b",
     "a157eb51a42817dd1ae4bc46cecf66e591ab32641df936da292bd09f2728631b"},
    {"source.spm", 0, "173e9f493a668fe396d599e28d414a201193094e6ffd7a4678e5aab0f6d3d838",
     "78d0e717c77053f1c4b856d8661d9cb87c64f083a35418c087b9146300e4f585"},
    {"target.spm", 0, "78d0e717c77053f1c4b856d8661d9cb87c64f083a35418c087b9146300e4f585",
     "173e9f493a668fe396d599e28d414a201193094e6ffd7a4678e5aab0f6d3d838"},
};
constexpr i64 kHelperBytes = 411648;
constexpr i64 kWheelBytes = 20407479;
constexpr char kHelperSha256[] = "8de072dd9474064094b0459cb950d8aa0f80283ca6a79e77c4fa0373aac07e51";
constexpr char kWheelSha256[] = "9667fa5589dec8b57eaa31d4602935259092e4185e016afb0c7d4a5efaaaedfb";
// the .spm files are small: their size only weights the progress bar
constexpr i64 kSmallFileBytes = 800 * 1024;

struct TranslateInstall {
    void (*onProgress)(int percent) = nullptr;
    void (*onDone)(bool ok) = nullptr;
    DownloadProgress download;
    i64 totalBytes = 1;
    i64 doneBytes = 0;
};

static TranslateInstall* gInstall = nullptr;

static bool Fetch(TranslateInstall* in, const char* url, Str dest, const char* sha256, i64 bytes) {
    if (file::Exists(dest)) {
        in->doneBytes += bytes;
        return true;
    }
    in->download.onProgress = in->onProgress;
    in->download.base = (int)(in->doneBytes * 100 / in->totalBytes);
    in->download.share = (int)(bytes * 100 / in->totalBytes);
    in->download.expectedBytes = bytes > 0 ? bytes : 1;
    bool ok = DownloadVerified(Str(url), dest, Str(sha256), &in->download);
    in->doneBytes += bytes;
    return ok;
}

// the two DLLs of the wheel, next to the helper
static bool ExtractWheelDlls(Str wheelPath, Str dir) {
    Archive archive;
    if (!archive.Open(wheelPath, true, FileType::Unknown, ArchiveExtractProgressCb())) {
        return false;
    }
    int found = 0;
    for (Archive::FileInfo* fi : archive.GetFileInfos()) {
        if (fi->isDir || !str::StartsWith(fi->name, Str(kWheelDllPrefix))) {
            continue;
        }
        Str rel = Str(fi->name.s + kWheelDllPrefixLen, len(fi->name) - kWheelDllPrefixLen);
        if (!str::EqI(rel, Str(kCt2Dll)) && !str::EqI(rel, Str(kOmpDll))) {
            continue;
        }
        Archive::FileInfo* data = archive.GetFileDataById(fi->fileId);
        if (!data || !data->data ||
            !file::WriteFile(path::JoinTemp(dir, rel), Str(data->data, data->fileSizeUncompressed))) {
            return false;
        }
        found++;
    }
    return found == 2;
}

static bool FetchModel(TranslateInstall* in, const char* base, bool enFr, Str modelDir) {
    for (const ModelFile& f : kModelFiles) {
        i64 bytes = f.bytes > 0 ? f.bytes : kSmallFileBytes;
        TempStr url = str::JoinTemp(Str(base), Str(f.name));
        if (!Fetch(in, url.s, path::JoinTemp(modelDir, Str(f.name)), enFr ? f.sha256EnFr : f.sha256FrEn, bytes)) {
            return false;
        }
    }
    return true;
}

static void InstallRun(TranslateInstall* in) {
    TempStr dir = TranslateDirTemp();
    TempStr wheel = path::JoinTemp(dir, Str(kWheelDest));
    bool ok = true;

    if (!file::Exists(InDir(Str(kCt2Dll))) || !file::Exists(InDir(Str(kOmpDll)))) {
        ok = Fetch(in, kWheelUrl, wheel, kWheelSha256, kWheelBytes) && ExtractWheelDlls(wheel, dir);
        file::Delete(wheel);
    } else {
        in->doneBytes += kWheelBytes;
    }
    ok = ok && Fetch(in, kHelperUrl, InDir(Str(kHelperExe)), kHelperSha256, kHelperBytes);
    ok = ok && FetchModel(in, kEnFrBase, true, ModelDirTemp(TranslateDir::EnToFr));
    ok = ok && FetchModel(in, kFrEnBase, false, ModelDirTemp(TranslateDir::FrToEn));
    logf("translate: install done ok=%d\n", (int)ok);
    in->onDone(ok);
}

static DWORD WINAPI InstallThread(void* arg) {
    auto* in = (TranslateInstall*)arg;
    InstallRun(in);
    gInstall = nullptr;
    delete in;
    ResetTempArena();
    return 0;
}

// one install at a time; the callbacks run on the worker thread
bool TranslateInstallStart(void (*onProgress)(int percent), void (*onDone)(bool ok)) {
    if (gInstall) {
        return false;
    }
    auto* in = new TranslateInstall();
    in->onProgress = onProgress;
    in->onDone = onDone;
    in->totalBytes = kWheelBytes + kHelperBytes;
    for (const ModelFile& f : kModelFiles) {
        in->totalBytes += 2 * (f.bytes > 0 ? f.bytes : kSmallFileBytes);
    }
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
