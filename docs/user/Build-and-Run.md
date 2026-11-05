# Build and run

## What you need

- Windows 10 or 11, x64.
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC, MSBuild, and the LLVM tools that provide `clang-format`). `bun cmd/build.ts` finds Visual Studio itself.
- [Bun](https://bun.sh), which runs the build scripts.
- Git.

## Get the code

```
git clone https://github.com/Artkanoid/sumatrapdf.git
cd sumatrapdf
```

## Build

```
bun cmd/build.ts -dbg    # debug
bun cmd/build.ts -rel    # release
```

Run `bun cmd/build.ts -help` for the other modes (`-32`, `-asan`, `-all`, `-clean`, `-ninja`).

A first build takes a few minutes (a release build took about 2.5 minutes here); later builds only rebuild what changed.

## Get the executable

| Build   | Folder       | Main file                  |
| ------- | ------------ | -------------------------- |
| debug   | `out\dbg64\` | `out\dbg64\SumatraPDF.exe` |
| release | `out\rel64\` | `out\rel64\SumatraPDF.exe` |

`SumatraPDF.exe` is a single file: it carries its libraries inside and unpacks them to `%LOCALAPPDATA%\SumatraPDF-data` on first start. Copy only that file to install it anywhere (checked: it starts alone from an empty folder).

The same folder also holds `libsumatrapdf.dll`, `PdfFilter.dll`, `PdfPreview.dll` (the search and preview handlers of the installer) and `sumatrapdf-tool.exe`. You don't need them to just run the program.

`bun cmd/build.ts -all` also builds `SumatraPDF-static.exe`, a statically linked variant.

## Run

```
out\dbg64\SumatraPDF.exe -for-testing "C:\path\to\file.pdf"
```

`-for-testing` starts a separate instance, doesn't restore the previous session and doesn't save settings, so it never touches your own settings. Without it the program behaves like a normal install.

The settings file is `SumatraPDF-settings.txt` in the program's data folder. Next to the debug build it is `out\dbg64\SumatraPDF-settings.txt`, and it is read even with `-for-testing`.

To keep a log: `-log -log-to-file C:\path\log.txt`.

## SumatraPDF ng (new interface)

```
bun cmd/ng-build.ts -dbg    # out\win\dbg\SumatraPDF.exe
bun cmd/ng-build.ts -rel    # out\win\rel
bun cmd/ng-build.ts -dbg -run -- -for-testing
```

`-linux`, `-mac` and `-wasm` build for the other platforms; see `bun cmd/ng-build.ts -help`.

## Tests

```
bun cmd/run-unit-tests.ts -dbg                       # SumatraPDF
bun cmd/ng-build.ts -dbg test_util -run -- -for-ai   # ng
```

## Format the code

After changing C or C++ files in `src/`:

```
bun cmd/format.ts
```

## Regenerate the keyboard shortcuts page

```
bun cmd/gen-shortcuts-doc.ts
```

It reads the default shortcuts from `src/shared/Accelerators.cpp` and fails if one key is bound to two commands. See [Keyboard shortcuts](Keyboard-Shortcuts.md).
