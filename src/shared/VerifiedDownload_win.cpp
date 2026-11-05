/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "base/Http.h"

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include "VerifiedDownload.h"

#include "SumatraLog.h"

constexpr char kPartExt[] = ".part";

static bool Sha256HexMatches(Str data, Str expectedHex) {
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
    return str::EqI(Str(hex), expectedHex);
}

static void OnDownload(DownloadProgress* in, HttpProgress* p) {
    i64 done = p->nDownloaded < in->expectedBytes ? p->nDownloaded : in->expectedBytes;
    in->onProgress(in->base + (int)(done * in->share / in->expectedBytes));
}

// Downloads to dest.part, checks the SHA-256, then moves it to dest, so a
// file that is there is a whole, expected one.
bool DownloadVerified(Str url, Str dest, Str sha256Hex, DownloadProgress* progress) {
    TempStr part = str::JoinTemp(dest, Str(kPartExt));
    dir::CreateForFile(dest);
    auto cb = MkFunc1(OnDownload, progress);
    if (!HttpGetToFile(url, part, cb)) {
        logf("download failed '%s'\n", url);
        file::Delete(part);
        return false;
    }
    Str data = file::ReadFile(part);
    bool ok = len(data) > 0 && Sha256HexMatches(data, sha256Hex);
    str::Free(data);
    if (!ok) {
        logf("download: hash mismatch '%s'\n", url);
        file::Delete(part);
        return false;
    }
    return MoveFileExW(CWStrTemp(part), CWStrTemp(dest), MOVEFILE_REPLACE_EXISTING) != 0;
}

// a name from an archive must stay inside the folder it is extracted to
bool NameIsSafeForExtract(Str name) {
    if (len(name) == 0 || str::IndexOf(name, StrL("..")) >= 0 || str::IndexOfChar(name, ':') >= 0) {
        return false;
    }
    return name.s[0] != '/' && name.s[0] != '\\';
}
