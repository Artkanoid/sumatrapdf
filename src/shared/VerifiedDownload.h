/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct DownloadProgress {
    void (*onProgress)(int percent) = nullptr;
    int base = 0;  // percent done before this download
    int share = 0; // percent this download adds
    i64 expectedBytes = 1;
};

bool DownloadVerified(Str url, Str dest, Str sha256Hex, DownloadProgress* progress);
bool NameIsSafeForExtract(Str name);
