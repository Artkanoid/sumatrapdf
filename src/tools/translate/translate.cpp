// Offline text translation helper for SumatraPDF.
//
//   sumatrapdf-translate.exe <model dir> [threads]
//
// stdin: UTF-8 text, one paragraph per line. A line holding only a form feed
// (0x0C) asks for the pending lines to be translated and written out, followed
// by a line holding only a form feed. EOF flushes too.
// stdout: one translated line per input line (an empty line stays empty).
//
// Models are CTranslate2 Opus-MT models (model.bin, source.spm, target.spm).

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

constexpr size_t kMaxSentenceChars = 360;
constexpr size_t kBatchSentences = 16;
constexpr char kFlush = '\f';

struct Line {
    std::vector<size_t> sentences; // indexes into the sentence list
};

static bool IsSentenceEnd(char c) {
    return c == '.' || c == '!' || c == '?';
}

// Cuts a paragraph where a sentence ends and a space follows, or when a piece
// gets too long for the model.
static std::vector<std::string> SplitSentences(const std::string& text) {
    std::vector<std::string> res;
    std::string cur;
    for (size_t i = 0; i < text.size(); i++) {
        cur += text[i];
        bool atEnd = i + 1 < text.size() && IsSentenceEnd(text[i]) && text[i + 1] == ' ';
        bool tooLong = cur.size() >= kMaxSentenceChars && text[i] == ' ';
        if (atEnd || tooLong) {
            res.push_back(cur);
            cur.clear();
            if (atEnd) {
                i++; // the space between two sentences
            }
        }
    }
    if (!cur.empty()) {
        res.push_back(cur);
    }
    return res;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

struct Engine {
    ctranslate2::Translator* translator = nullptr;
    sentencepiece::SentencePieceProcessor source;
    sentencepiece::SentencePieceProcessor target;
};

static std::vector<std::string> TranslateSentences(Engine& e, const std::vector<std::string>& sentences) {
    std::vector<std::string> out(sentences.size());
    for (size_t start = 0; start < sentences.size(); start += kBatchSentences) {
        size_t end = std::min(start + kBatchSentences, sentences.size());
        std::vector<std::vector<std::string>> batch;
        for (size_t i = start; i < end; i++) {
            std::vector<std::string> pieces;
            e.source.Encode(sentences[i], &pieces);
            pieces.push_back("</s>");
            batch.push_back(pieces);
        }
        ctranslate2::TranslationOptions opts;
        opts.beam_size = 2;
        auto results = e.translator->translate_batch(batch, opts, kBatchSentences);
        for (size_t i = start; i < end; i++) {
            std::string text;
            e.target.Decode(results[i - start].output(), &text);
            out[i] = text;
        }
    }
    return out;
}

static void Flush(Engine& e, const std::vector<std::string>& lines) {
    std::vector<std::string> sentences;
    std::vector<Line> index(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        std::string t = Trim(lines[i]);
        if (t.empty()) {
            continue;
        }
        for (auto& s : SplitSentences(t)) {
            index[i].sentences.push_back(sentences.size());
            sentences.push_back(s);
        }
    }
    std::vector<std::string> translated = TranslateSentences(e, sentences);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string line;
        for (size_t k = 0; k < index[i].sentences.size(); k++) {
            if (k > 0) {
                line += ' ';
            }
            line += translated[index[i].sentences[k]];
        }
        fwrite(line.data(), 1, line.size(), stdout);
        fputc('\n', stdout);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sumatrapdf-translate <model dir> [threads]\n");
        return 2;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::string dir = argv[1];
    size_t threads = argc > 2 ? (size_t)atoi(argv[2]) : 0;

    Engine e;
    if (!e.source.Load(dir + "/source.spm").ok() || !e.target.Load(dir + "/target.spm").ok()) {
        fprintf(stderr, "cannot load the sentencepiece models\n");
        return 3;
    }
    ctranslate2::ReplicaPoolConfig pool;
    pool.num_threads_per_replica = threads;
    try {
        e.translator = new ctranslate2::Translator(dir, ctranslate2::Device::CPU, ctranslate2::ComputeType::INT8,
                                                   std::vector<int>{0}, false, pool);
    } catch (const std::exception& ex) {
        fprintf(stderr, "cannot load the model: %s\n", ex.what());
        return 4;
    }

    std::vector<std::string> pending;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() == 1 && line[0] == kFlush) {
            Flush(e, pending);
            pending.clear();
            fputc(kFlush, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            continue;
        }
        pending.push_back(line);
    }
    if (!pending.empty()) {
        Flush(e, pending);
    }
    fflush(stdout);
    return 0;
}
