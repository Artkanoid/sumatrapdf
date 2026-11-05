/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/UITask.h"
#include "base/File.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"

#include "SumatraConfig.h"
#include "Settings.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "Commands.h"
#include "Translations.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Notifications.h"
#include "Selection.h"
#include "SumatraPDF.h"
#if OS_WIN
#include "TranslateEngine.h"
#endif
#include "TranslateCommon.h"

#include "SumatraLog.h"

/*
Offline translation of the selection or of the whole document. The result is a
text file opened in a tab, so it can be read, searched and read aloud like any
other document. The engine is in TranslateEngine_win.cpp.
*/

#if OS_WIN

constexpr int kNotifMs = 4000;
// a line is the last of its paragraph when it ends a sentence and is clearly
// shorter than the longest line of the page
constexpr int kShortLinePercent = 75;
constexpr int kPercentFull = 100;
constexpr char kTranslationsDir[] = "translations";

struct TranslateRun {
    MainWindow* win = nullptr;
    TranslateJob* job = nullptr;
    NotificationWnd* notif = nullptr;
    Str title;
    Str original;
    bool isDocument = false;
    TranslateDir dir = TranslateDir::EnToFr;
};

static TranslateRun gRun;
static AtomicInt gPercent = 0;
static AtomicInt gPostedPercent = -1;
static AtomicInt gInstallOk = 0;

static void Notify(MainWindow* win, Str msg, bool warning = false) {
    NotificationCreateArgs args;
    args.win = win;
    args.msg = msg;
    args.warning = warning;
    args.timeoutMs = kNotifMs;
    ShowNotification(args);
}

// Turns the progress notification into the final message, which goes away by
// itself. CloseNotification queues a removal that dangles when the notification
// is removed first, and opening a document removes them.
static void EndProgress(MainWindow* win, NotificationWnd* notif, Str msg, bool warning, int percent) {
    if (notif && UpdateNotificationProgress(notif, msg, percent)) {
        NotificationUpdateMessage(notif, msg, kNotifMs);
        return;
    }
    Notify(win, msg, warning);
}

static bool IsEndOfSentence(char c) {
    return c == '.' || c == '!' || c == '?' || c == ':';
}

static bool IsLowerAscii(char c) {
    return c >= 'a' && c <= 'z';
}

// Text of a page is one line per visual line: joins them into paragraphs, one
// per output line, with an empty line between two.
static void AppendParagraphs(str::Builder& out, Str text) {
    Vec<Str> lines;
    int maxLen = 0;
    int start = 0;
    for (int i = 0; i <= len(text); i++) {
        if (i < len(text) && text.s[i] != '\n') {
            continue;
        }
        int end = i;
        if (end > start && text.s[end - 1] == '\r') {
            end--;
        }
        Str line = Str(text.s + start, end - start);
        VecAppend(lines, line);
        maxLen = end - start > maxLen ? end - start : maxLen;
        start = i + 1;
    }

    str::Builder para;
    for (Str line : lines) {
        int a = 0;
        int b = len(line);
        while (a < b && (line.s[a] == ' ' || line.s[a] == '\t')) {
            a++;
        }
        while (b > a && (line.s[b - 1] == ' ' || line.s[b - 1] == '\t')) {
            b--;
        }
        Str t = Str(line.s + a, b - a);
        bool endsParagraph = len(t) == 0;
        if (len(t) > 0) {
            Str cur = ToStr(para);
            int curLen = len(cur);
            bool hyphenated =
                curLen > 1 && cur.s[curLen - 1] == '-' && IsLowerAscii(cur.s[curLen - 2]) && IsLowerAscii(t.s[0]);
            if (hyphenated) {
                para.len--; // "cap-" + "turing" -> "capturing"
            } else if (curLen > 0) {
                para.AppendChar(' ');
            }
            para.Append(t);
            endsParagraph = IsEndOfSentence(t.s[len(t) - 1]) && len(t) * kPercentFull < maxLen * kShortLinePercent;
        }
        if (endsParagraph && para.len > 0) {
            out.Append(ToStr(para));
            out.Append(StrL("\n\n"));
            para.Reset();
        }
    }
    if (para.len > 0) {
        out.Append(ToStr(para));
        out.Append(StrL("\n\n"));
    }
}

static Str DocumentTextDup(WindowTab* tab) {
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return {};
    }
    EngineBase* engine = dm->GetEngine();
    str::Builder all;
    for (int pageNo = 1; pageNo <= engine->PageCount(); pageNo++) {
        PageText pt = engine->ExtractPageText(pageNo);
        AppendParagraphs(all, pt.text);
        FreePageText(&pt);
    }
    return str::Dup(ToStr(all));
}

static void RunReset() {
    str::Free(gRun.title);
    str::Free(gRun.original);
    gRun = TranslateRun();
    AtomicIntSet(&gPostedPercent, -1);
}

static TempStr SafeFileNameTemp(Str s) {
    TempStr res = str::DupTemp(s);
    for (int i = 0; i < len(res); i++) {
        char c = res.s[i];
        bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                   c == '|' || (unsigned char)c < ' ';
        if (bad) {
            res.s[i] = '_';
        }
    }
    return res;
}

static void RunUiProgress(void*) {
    if (!gRun.job || !gRun.notif) {
        return;
    }
    int perc = AtomicIntGet(&gPercent);
    UpdateNotificationProgress(gRun.notif, fmt(Tr("Translating... %d%%").s, perc), perc);
}

static void RunUiDone(void*) {
    MainWindow* win = gRun.win;
    TranslateJob* job = gRun.job;
    NotificationWnd* notif = gRun.notif;
    if (!job) {
        return; // cancelled
    }
    gRun.job = nullptr;
    gRun.notif = nullptr;
    if (!IsMainWindowValidAndNotClosing(win)) {
        TranslateJobCancel(job);
        RunReset();
        return;
    }
    Str translated = {};
    if (!TranslateJobTake(job, &translated)) {
        EndProgress(win, notif, Tr("The translation failed"), true, 0);
        RunReset();
        return;
    }

    str::Builder out;
    if (!gRun.isDocument) {
        out.Append(Tr("== Original =="));
        out.Append(StrL("\n\n"));
        out.Append(gRun.original);
        out.Append(StrL("\n\n"));
        out.Append(Tr("== Translation =="));
        out.Append(StrL("\n\n"));
    }
    out.Append(translated);
    str::Free(translated);

    Str lang = gRun.dir == TranslateDir::EnToFr ? StrL("fr") : StrL("en");
    TempStr name = fmt("%s.%s.txt", SafeFileNameTemp(gRun.title), lang);
    TempStr path = GetPathInAppDataDirTemp(path::JoinTemp(Str(kTranslationsDir), name));
    dir::CreateForFile(path);
    bool ok = file::WriteFile(path, ToStr(out));
    RunReset();
    if (!ok) {
        EndProgress(win, notif, Tr("The translation failed"), true, 0);
        return;
    }
    EndProgress(win, notif, Tr("Translation ready"), false, kPercentFull);
    LoadDocument(win, path);
}

static void OnRunProgress(int percent) {
    AtomicIntSet(&gPercent, percent);
    if (AtomicIntGet(&gPostedPercent) != percent) {
        AtomicIntSet(&gPostedPercent, percent);
        uitask::Post(MkFunc0(RunUiProgress, (void*)nullptr));
    }
}

static void OnRunDone() {
    uitask::Post(MkFunc0(RunUiDone, (void*)nullptr));
}

// text: paragraphs, one per line; takes the strings (title, original) over
static void StartRun(MainWindow* win, Str text, Str title, Str original, bool isDocument) {
    if (!TranslateInstalled()) {
        Notify(win, Tr("Install the offline translation first (command: Install Offline Translation)"), true);
        str::Free(title);
        str::Free(original);
        return;
    }
    if (gRun.job) {
        Notify(win, Tr("A translation is already running"), true);
        str::Free(title);
        str::Free(original);
        return;
    }
    gRun.win = win;
    gRun.title = title;
    gRun.original = original;
    gRun.isDocument = isDocument;
    gRun.dir = TranslateGuessDir(text);

    NotificationCreateArgs args;
    args.win = win;
    args.msg = fmt(Tr("Translating... %d%%").s, 0);
    gRun.notif = ShowNotification(args);
    AtomicIntSet(&gPercent, 0);
    gRun.job = TranslateStart(gRun.dir, text, OnRunProgress, OnRunDone);
    if (gRun.job) {
        return;
    }
    NotificationWnd* notif = gRun.notif;
    RunReset();
    EndProgress(win, notif, Tr("The translation failed"), true, 0);
}

void TranslateSelectionOffline(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !HasPermission(Perm::CopySelection)) {
        return;
    }
    bool isTextOnly = false;
    TempStr selected = GetSelectedTextTemp(tab, StrL("\n"), isTextOnly);
    if (len(selected) == 0) {
        Notify(win, Tr("Select some text to translate first"), true);
        return;
    }
    str::Builder paragraphs;
    AppendParagraphs(paragraphs, selected);
    Str name = path::GetBaseNameTemp(tab->filePath);
    StartRun(win, ToStr(paragraphs), str::Dup(name), str::Dup(selected), false);
}

void TranslateDocumentOffline(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !HasPermission(Perm::CopySelection)) {
        return;
    }
    Str text = DocumentTextDup(tab);
    if (len(text) == 0) {
        Notify(win, Tr("No text available to translate"), true);
        return;
    }
    Str name = path::GetBaseNameTemp(tab->filePath);
    StartRun(win, text, str::Dup(name), {}, true);
    str::Free(text);
}

void TranslateCancelOffline(MainWindow* win) {
    if (!gRun.job) {
        return;
    }
    TranslateJobCancel(gRun.job);
    NotificationWnd* notif = gRun.notif;
    RunReset();
    EndProgress(win, notif, Tr("Translation cancelled"), false, 0);
}

//--- install

static MainWindow* gInstallWin = nullptr;
static NotificationWnd* gInstallNotif = nullptr;

static void InstallUiProgress(void*) {
    if (!IsMainWindowValidAndNotClosing(gInstallWin) || !gInstallNotif) {
        return;
    }
    int perc = AtomicIntGet(&gPercent);
    UpdateNotificationProgress(gInstallNotif, fmt(Tr("Downloading offline translation... %d%%").s, perc), perc);
}

static void InstallUiDone(void*) {
    MainWindow* win = gInstallWin;
    NotificationWnd* notif = gInstallNotif;
    gInstallWin = nullptr;
    gInstallNotif = nullptr;
    if (!IsMainWindowValidAndNotClosing(win)) {
        return;
    }
    bool ok = AtomicIntGet(&gInstallOk) != 0;
    Str msg = ok ? Tr("Offline translation installed") : Tr("Could not install the offline translation");
    EndProgress(win, notif, msg, !ok, ok ? kPercentFull : 0);
}

static void InstallOnProgress(int percent) {
    AtomicIntSet(&gPercent, percent);
    if (AtomicIntGet(&gPostedPercent) != percent) {
        AtomicIntSet(&gPostedPercent, percent);
        uitask::Post(MkFunc0(InstallUiProgress, (void*)nullptr));
    }
}

static void InstallOnDone(bool ok) {
    AtomicIntSet(&gInstallOk, ok ? 1 : 0);
    uitask::Post(MkFunc0(InstallUiDone, (void*)nullptr));
}

void TranslateInstallOffline(MainWindow* win) {
    if (!win) {
        return;
    }
    if (TranslateInstalled()) {
        Notify(win, Tr("Offline translation is already installed"));
        return;
    }
    if (!HasPermission(Perm::InternetAccess) || !HasPermission(Perm::DiskAccess)) {
        Notify(win, Tr("Downloading is disabled by the restrictions"), true);
        return;
    }
    NotificationCreateArgs args;
    args.win = win;
    args.msg = fmt(Tr("Downloading offline translation... %d%%").s, 0);
    gInstallNotif = ShowNotification(args);
    gInstallWin = win;
    AtomicIntSet(&gPostedPercent, -1);
    if (TranslateInstallStart(InstallOnProgress, InstallOnDone)) {
        return;
    }
    NotificationWnd* notif = gInstallNotif;
    gInstallNotif = nullptr;
    EndProgress(win, notif, Tr("Could not install the offline translation"), true, 0);
}

#else

static void NotWindows(MainWindow* win) {
    NotificationCreateArgs args;
    args.win = win;
    args.msg = Tr("Offline translation is only available on Windows");
    args.timeoutMs = 3000;
    ShowNotification(args);
}

void TranslateInstallOffline(MainWindow* win) {
    NotWindows(win);
}

void TranslateSelectionOffline(MainWindow* win) {
    NotWindows(win);
}

void TranslateDocumentOffline(MainWindow* win) {
    NotWindows(win);
}

void TranslateCancelOffline(MainWindow*) {}

#endif
