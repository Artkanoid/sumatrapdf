# Read Aloud

Read Aloud reads the document with the voices installed on your system. The text stays on your computer.

Start it from the **Read Aloud** toolbar button, the **Read Aloud** submenu of the right-click menu, or the command palette. A playback bar appears at the bottom of the window with Pause / Resume, Stop, Voice, volume and speed.

Not available for comics, image folders and single images (no text to read), and for documents that don't allow copying text.

## Where to start reading

| What                             | How                                                                                                                       |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| From the top of the visible page | **Start Reading From Top**, `Ctrl+Shift+Space`                                                                            |
| From the pointer                 | **Start Reading From Cursor Position** (right-click), `Ctrl+Alt+Click`, `Ctrl+Shift+Enter` (reads from the mouse pointer) |
| The selected text                | **Start Reading Selection**, `Alt+Shift+R`                                                                                |
| Toggle (start, pause, continue)  | `Ctrl+Space` or `Ctrl+Shift+R`                                                                                            |
| Stop                             | **Stop Reading**, `Alt+Shift+S`                                                                                           |

## Moving while reading

| What                      | Keyboard                           | Mouse                      |
| ------------------------- | ---------------------------------- | -------------------------- |
| Previous / next sentence  | `Ctrl+Alt+Left` / `Ctrl+Alt+Right` | `Ctrl+Alt+Wheel` up / down |
| Previous / next paragraph | `Ctrl+Alt+Up` / `Ctrl+Alt+Down`    |                            |

`Ctrl+Alt+Wheel` zooms as usual when nothing is being read.

## Playback bar

- **Volume:** the speaker icon shows the level (high, low, muted). Click it to mute and click again to restore. Drag the slider to change the level. Saved in the `ReadAloudVolume` setting. Not supported yet by the web (wasm) build.
- **Speed:** 0.5x to 3x, saved in `ReadAloudSpeed`.
- **Voice:** pick any voice installed on the system.

## Follow along

While reading, the sentence being spoken is underlined, and the word on top of it. Change this in the advanced settings:

```
ReadAloudHighlight = true
ReadAloudWordColor = #f59e0b
ReadAloudSentenceColor = #3b82f6
```

## Natural voices (Windows)

Run **Read Aloud: Install Natural Voice** from the command palette to get a neural French voice ([Piper](https://github.com/rhasspy/piper), voice _siwis_). It downloads about 85 MB once, checks the files, and selects the voice. Everything runs on your computer: the text is never sent anywhere. The download needs internet access and disk access (not blocked by restrictions).

- Installed in the `piper` folder of the SumatraPDF data folder. Delete that folder to remove it.
- Other Piper voices: put the `.onnx` and `.onnx.json` files of a voice in `piper/voices`. They appear in the **Voice** menu.
- Uses about 240 MB of memory while speaking. The word being spoken is estimated, so the word underline is less precise than with the Windows voices.

## New commands

- `CmdReadAloudNextSentence` : "Read Aloud: Next Sentence" (`Ctrl+Alt+Right`)
- `CmdReadAloudPrevSentence` : "Read Aloud: Previous Sentence" (`Ctrl+Alt+Left`)
- `CmdReadAloudNextParagraph` : "Read Aloud: Next Paragraph" (`Ctrl+Alt+Down`)
- `CmdReadAloudPrevParagraph` : "Read Aloud: Previous Paragraph" (`Ctrl+Alt+Up`)
- `CmdReadAloudInstallVoice` : "Read Aloud: Install Natural Voice" (palette only)

All shortcuts can be rebound, see the keyboard shortcuts documentation.

## New advanced settings

- `ReadAloudVolume` : volume, 0 to 1, default 1
- `ReadAloudHighlight` : underline the sentence and word being spoken, default true
- `ReadAloudWordColor` : color of the word underline, default `#f59e0b`
- `ReadAloudSentenceColor` : color of the sentence underline, default `#3b82f6`
