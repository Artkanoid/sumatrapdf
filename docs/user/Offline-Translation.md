# Offline translation

Translates the selected text or the whole document between English and French, on your computer. The text is never sent anywhere.

The translation opens in a new tab as a text document, so you can read it, search it, copy it and have it read aloud (see [Read Aloud](Read-Aloud.md)).

## Commands

Run them from the command palette (`Ctrl+K`):

| Command                           | What it does                                                                        |
| --------------------------------- | ----------------------------------------------------------------------------------- |
| **Install Offline Translation**   | Downloads the translator and the two models (about 180 MB, once).                   |
| **Translate Selection (Offline)** | Translates the selected text. The new tab shows the original, then the translation. |
| **Translate Document (Offline)**  | Translates every page. The new tab holds the translated text.                       |
| **Cancel Offline Translation**    | Stops a translation in progress.                                                    |

The language is detected from the text: English is translated to French, French to English. A progress notification shows the percentage.

The result is saved as `<name>.fr.txt` or `<name>.en.txt` in the `translations` folder of the SumatraPDF data folder.

## What it needs

- About 212 MB on disk, in the `translate` folder of the data folder. Delete the folder to remove it.
- About 170 MB of memory while translating. Nothing is loaded when you don't translate.
- Speed: about 3 000 characters per second on one core, 7 000 on four (measured on a fast desktop processor). A 300 page book takes a few minutes; an older computer is slower. A selection or a page is instant.
- Internet access and disk access, for the install only (not blocked by restrictions).

## Limits

- English and French only for now.
- The text of a PDF page is rebuilt into paragraphs from its lines, which is not always right for tables, columns or headings. The layout of the original is not kept.
- Documents without text (scans) can't be translated.

## What is installed

Every download is checked against a SHA-256 fixed in the program.

- The translator: `sumatrapdf-translate.exe`, a small program built from `src/tools/translate/translate.cpp` that uses [CTranslate2](https://github.com/OpenNMT/CTranslate2) (MIT license) and [SentencePiece](https://github.com/google/sentencepiece) (Apache 2.0). Its DLLs come from the official CTranslate2 wheel on PyPI.
- Two [Opus-MT](https://github.com/Helsinki-NLP/Opus-MT) models by Helsinki-NLP (CC BY 4.0), converted for CTranslate2 by a third party on Hugging Face.

## For maintainers

`bun cmd/build-translate.ts` builds the translator into `out/translate/`. Upload `sumatrapdf-translate.exe` to the release named in `src/shared/TranslateEngine_win.cpp` (`kHelperUrl`) and set `kHelperSha256` and `kHelperBytes` to the values the script prints.
