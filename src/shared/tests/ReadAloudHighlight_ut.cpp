/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "ChapterTable.h"
#include "ReadAloud.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

void ReadAloudHighlight_UnitTests() {
    int s = 0;
    int e = 0;

    Str two = StrL("Hello world. Next one!");
    int nextAt = str::IndexOf(two, StrL("Next"));
    utassert(nextAt >= 0);
    utassert(ReadAloudSentenceRange(two, 0, &s, &e));
    utassert(s == 0);
    utassert(e == nextAt);
    utassert(ReadAloudSentenceRange(two, 6, &s, &e));
    utassert(s == 0);
    utassert(e == nextAt);
    utassert(ReadAloudSentenceRange(two, nextAt, &s, &e));
    utassert(s == nextAt);
    utassert(e == two.len);

    Str abbr = StrL("See e.g. the cat. Done.");
    int theAt = str::IndexOf(abbr, StrL("the"));
    int doneAt = str::IndexOf(abbr, StrL("Done"));
    utassert(theAt >= 0 && doneAt >= 0);
    utassert(ReadAloudSentenceRange(abbr, theAt, &s, &e));
    utassert(s == 0);
    utassert(e == doneAt);
    utassert(ReadAloudSentenceRange(abbr, doneAt, &s, &e));
    utassert(s == doneAt);
    utassert(e == abbr.len);

    Str quoted = StrL("He said \"Go!\" Then left.");
    int thenAt = str::IndexOf(quoted, StrL("Then"));
    utassert(thenAt >= 0);
    utassert(ReadAloudSentenceRange(quoted, 0, &s, &e));
    utassert(s == 0);
    utassert(e == thenAt);
    utassert(ReadAloudSentenceRange(quoted, thenAt, &s, &e));
    utassert(s == thenAt);
    utassert(e == quoted.len);

    Str para = StrL("First  Second");
    int secondAt = str::IndexOf(para, StrL("Second"));
    utassert(secondAt >= 0);
    utassert(ReadAloudSentenceRange(para, 0, &s, &e));
    utassert(s == 0);
    utassert(e == secondAt - 1);
    utassert(ReadAloudSentenceRange(para, secondAt, &s, &e));
    utassert(s == secondAt);
    utassert(e == para.len);

    Str dotted = StrL("First.  Second.");
    int dottedSecond = str::IndexOf(dotted, StrL("Second"));
    utassert(dottedSecond >= 0);
    utassert(ReadAloudSentenceRange(dotted, 0, &s, &e));
    utassert(s == 0);
    utassert(e == dottedSecond);
    utassert(ReadAloudSentenceRange(dotted, dottedSecond, &s, &e));
    utassert(s == dottedSecond);
    utassert(e == dotted.len);

    // U+3002 ideographic full stop
    Str cjk = StrL("你好。世界");
    utassert(ReadAloudSentenceRange(cjk, 0, &s, &e));
    utassert(s == 0);
    utassert(e > 0);
    utassert(e < cjk.len);
    utassert(ReadAloudSentenceRange(cjk, e, &s, &e));
    utassert(e == cjk.len);

    // sentences: "One two. Three four.  Five six. Seven."
    Str doc = StrL("One two. Three four.  Five six. Seven.");
    int three = str::IndexOf(doc, StrL("Three"));
    int five = str::IndexOf(doc, StrL("Five"));
    int six = str::IndexOf(doc, StrL("six"));
    int seven = str::IndexOf(doc, StrL("Seven"));
    ReadAloudJump nextS = ReadAloudJump::NextSentence;
    ReadAloudJump prevS = ReadAloudJump::PrevSentence;
    ReadAloudJump nextP = ReadAloudJump::NextParagraph;
    ReadAloudJump prevP = ReadAloudJump::PrevParagraph;
    utassert(ReadAloudJumpPos(doc, 0, nextS) == three);
    utassert(ReadAloudJumpPos(doc, three + 2, nextS) == five);
    utassert(ReadAloudJumpPos(doc, six, nextS) == seven);
    utassert(ReadAloudJumpPos(doc, seven, nextS) == -1);
    utassert(ReadAloudJumpPos(doc, six, prevS) == five);
    utassert(ReadAloudJumpPos(doc, five, prevS) == three);
    utassert(ReadAloudJumpPos(doc, three, prevS) == 0);
    utassert(ReadAloudJumpPos(doc, 0, prevS) == 0);

    // paragraphs
    utassert(ReadAloudJumpPos(doc, 0, nextP) == five);
    utassert(ReadAloudJumpPos(doc, three, nextP) == five);
    utassert(ReadAloudJumpPos(doc, five, nextP) == -1);
    utassert(ReadAloudJumpPos(doc, seven, prevP) == five);
    utassert(ReadAloudJumpPos(doc, five, prevP) == 0);
    utassert(ReadAloudJumpPos(doc, three, prevP) == 0);
    utassert(ReadAloudJumpPos(StrL(""), 0, nextS) == -1);
}

#if !defined(SUMATRA_NG)
void ReadAloudSession_UnitTests() {
    // chunking prefers a sentence end, then a space, then the hard limit
    Str sents = StrL("Aaa bbb. Ccc ddd eee.");
    int cccAt = str::IndexOf(sents, StrL("Ccc"));
    utassert(ReadAloudFindChunkEnd(sents, 0, 12) == cccAt);
    utassert(ReadAloudFindChunkEnd(sents, 0, 100) == sents.len);
    utassert(ReadAloudFindChunkEnd(sents, sents.len, 12) == sents.len);

    Str abbr = StrL("See e.g. the cat sits");
    int theAt = str::IndexOf(abbr, StrL("the"));
    utassert(ReadAloudFindChunkEnd(abbr, 0, 12) == theAt);

    utassert(ReadAloudFindChunkEnd(StrL("abcdefghij"), 0, 4) == 4);

    // text cleanup: hyphenation, line breaks, paragraph breaks, spaces
    utassert(str::Eq(CleanReadAloudTextTemp(StrL("cap-\nturing")), StrL("capturing")));
    utassert(str::Eq(CleanReadAloudTextTemp(StrL("X-\nbar")), StrL("X- bar")));
    utassert(str::Eq(CleanReadAloudTextTemp(StrL("a\nb")), StrL("a b")));
    utassert(str::Eq(CleanReadAloudTextTemp(StrL("a\n\nb")), StrL("a  b")));
    utassert(str::Eq(CleanReadAloudTextTemp(StrL("a \t b")), StrL("a b")));
    utassert(len(CleanReadAloudTextTemp(StrL(""))) == 0);

    // speed presets
    utassert(str::Eq(ReadAloudSpeedLabelTemp(1.0f), StrL("1x")));
    utassert(str::Eq(ReadAloudSpeedLabelTemp(1.5f), StrL("1.5x")));
    utassert(str::Eq(ReadAloudSpeedLabelTemp(0.75f), StrL("0.75x")));
    utassert(str::Eq(ReadAloudSpeedLabelTemp(2.25f), StrL("2.25x")));
    int n = ReadAloudSpeedCount();
    utassert(n > 1);
    utassert(ReadAloudSpeedAt(-5) == ReadAloudSpeedAt(0));
    utassert(ReadAloudSpeedAt(n + 5) == ReadAloudSpeedAt(n - 1));
    for (int i = 1; i < n; i++) {
        utassert(ReadAloudSpeedAt(i) > ReadAloudSpeedAt(i - 1));
    }
}
#endif
