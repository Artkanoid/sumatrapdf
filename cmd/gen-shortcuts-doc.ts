// Writes docs/user/Keyboard-Shortcuts.md from the default accelerator table in
// src/shared/Accelerators.cpp, with the command names of cmd/gen-commands.ts.
// Fails when a key is bound to two commands or a bound command has no group.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dir, "..");
const accelPath = join(root, "src", "shared", "Accelerators.cpp");
const commandsPath = join(root, "cmd", "gen-commands.ts");
const outPath = join(root, "docs", "user", "Keyboard-Shortcuts.md");

const groups: [string, string[]][] = [
  [
    "Scrolling and pages",
    [
      "CmdScrollUp",
      "CmdScrollDown",
      "CmdScrollLeft",
      "CmdScrollRight",
      "CmdScrollUpHalfPage",
      "CmdScrollDownHalfPage",
      "CmdScrollLeftPage",
      "CmdScrollRightPage",
      "CmdScrollUpPage",
      "CmdScrollDownPage",
      "CmdGoToNextPage",
      "CmdGoToPrevPage",
      "CmdGoToFirstPage",
      "CmdGoToLastPage",
      "CmdGoToPage",
      "CmdNavigateBack",
      "CmdNavigateForward",
    ],
  ],
  [
    "Files and windows",
    [
      "CmdOpenFile",
      "CmdOpenNextFileInFolder",
      "CmdOpenPrevFileInFolder",
      "CmdNavigateFilesInFolder",
      "CmdRenameFile",
      "CmdNewWindow",
      "CmdDuplicateInNewWindow",
      "CmdSaveAs",
      "CmdPrint",
      "CmdReloadDocument",
      "CmdReopenLastClosedFile",
      "CmdProperties",
      "CmdFavoriteAdd",
      "CmdClose",
      "CmdCloseCurrentDocument",
      "CmdExit",
    ],
  ],
  [
    "Edit and selection",
    [
      "CmdSelectAll",
      "CmdCopySelection",
      "CmdPasteClipboardImage",
      "CmdCutAnnotation",
      "CmdUndo",
      "CmdRedo",
      "CmdDeleteAnnotation",
      "CmdSaveAnnotations",
      "CmdSelectTextViaKeyboard",
      "CmdCreateAnnotHighlight",
      "CmdCreateAnnotUnderline",
    ],
  ],
  ["Find", ["CmdFindFirst", "CmdFindNext", "CmdFindPrev", "CmdFindNextSel", "CmdFindPrevSel", "CmdFindHistory"]],
  [
    "Command palette",
    [
      "CmdCommandPalette",
      "CmdNavigateTabs",
      "CmdNavigateHistory",
      "CmdNavigateFavorites",
      "CmdNavigateTableOfContents",
      "CmdNavigateThumbnail",
      "CmdQuickEditSettings",
    ],
  ],
  [
    "Zoom, layout and rotation",
    [
      "CmdZoomCustom",
      "CmdZoomFitPage",
      "CmdZoomActualSize",
      "CmdZoomFitWidth",
      "CmdZoomFitContent",
      "CmdZoomToSelection",
      "CmdZoomIn",
      "CmdZoomOut",
      "CmdToggleZoom",
      "CmdSinglePageView",
      "CmdFacingView",
      "CmdBookView",
      "CmdToggleContinuousView",
      "CmdRotateLeft",
      "CmdRotateRight",
      "CmdInvertColors",
    ],
  ],
  [
    "Window and display",
    [
      "CmdTogglePresentationMode",
      "CmdToggleFullscreen",
      "CmdToggleToolbar",
      "CmdToggleCaption",
      "CmdToggleMenuBar",
      "CmdToggleBookmarks",
      "CmdMoveFrameFocus",
      "CmdTogglePageInfo",
      "CmdToggleCursorPosition",
      "CmdToggleKeyboardLinkFollowing",
      "CmdToggleAutomaticallyScroll",
      "CmdPresentationWhiteBackground",
      "CmdPresentationBlackBackground",
      "CmdToggleKeyboardHelp",
      "CmdHelpOpenManual",
    ],
  ],
  ["Tabs", ["CmdNextTab", "CmdPrevTab", "CmdMoveTabRight", "CmdMoveTabLeft", "CmdNextTabSmart", "CmdPrevTabSmart"]],
  [
    "Read Aloud",
    [
      "CmdToggleReadAloud",
      "CmdReadAloudFromTopPage",
      "CmdReadAloudFromCursorPosition",
      "CmdReadAloudSelection",
      "CmdStopReadAloud",
      "CmdReadAloudPrevSentence",
      "CmdReadAloudNextSentence",
      "CmdReadAloudPrevParagraph",
      "CmdReadAloudNextParagraph",
    ],
  ],
];

const vkNames: Record<string, string> = {
  UP: "Up",
  DOWN: "Down",
  LEFT: "Left",
  RIGHT: "Right",
  NEXT: "Page Down",
  PRIOR: "Page Up",
  HOME: "Home",
  END: "End",
  SPACE: "Space",
  RETURN: "Enter",
  BACK: "Backspace",
  INSERT: "Insert",
  DELETE: "Delete",
  TAB: "Tab",
  ADD: "Numpad +",
  SUBTRACT: "Numpad -",
  OEM_PLUS: "+",
  OEM_MINUS: "-",
  OEM_2: "/",
  OEM_4: "[",
  OEM_6: "]",
};

type Binding = { mods: Set<string>; key: string; cmd: string };

function keyName(raw: string): string {
  raw = raw.trim();
  const ch = raw.match(/^'(.)'$/);
  if (ch) {
    return ch[1];
  }
  const vk = raw.match(/^VK_(\w+)$/);
  if (!vk) {
    throw new Error(`unknown key: ${raw}`);
  }
  const name = vk[1];
  if (vkNames[name]) {
    return vkNames[name];
  }
  if (/^F\d+$/.test(name)) {
    return name;
  }
  const pad = name.match(/^NUMPAD(\d)$/);
  if (pad) {
    return `Numpad ${pad[1]}`;
  }
  throw new Error(`unknown virtual key: ${raw}`);
}

// the Windows / Linux table only: no kCmd (Mac) entries
function parseBindings(): Binding[] {
  const src = readFileSync(accelPath, "utf8").replace(/\r\n/g, "\n");
  const start = src.indexOf("static Accel gBuiltInAccelerators[] = {");
  const end = src.indexOf("\n};", start);
  const body = src
    .slice(start, end)
    .split("\n")
    .filter((l) => !l.trim().startsWith("//") && !l.trim().startsWith("#"));
  const res: Binding[] = [];
  for (const line of body) {
    const m = line.match(/\{\{([^,]+),\s*([^}]+?)\},\s*(Cmd\w+)\}/);
    if (!m) {
      continue;
    }
    const mods = new Set<string>();
    for (const t of m[1].split("|").map((s) => s.trim())) {
      if (t === "kCtrl") mods.add("Ctrl");
      else if (t === "kAlt") mods.add("Alt");
      else if (t === "kShift") mods.add("Shift");
      else if (t !== "kVirt" && t !== "0") throw new Error(`unknown modifier ${t} in: ${line}`);
    }
    res.push({ mods, key: keyName(m[2]), cmd: m[3] });
  }
  return res;
}

function combo(b: Binding): string {
  const order = ["Ctrl", "Alt", "Shift"];
  const mods = order.filter((m) => b.mods.has(m));
  return [...mods, b.key].join("+");
}

function commandNames(): Map<string, string> {
  const src = readFileSync(commandsPath, "utf8");
  const res = new Map<string, string>();
  for (const m of src.matchAll(/"(Cmd\w+)",\s*"([^"]*)"/g)) {
    res.set(m[1], m[2]);
  }
  return res;
}

function main() {
  const bindings = parseBindings();
  const names = commandNames();

  const seen = new Map<string, string>();
  for (const b of bindings) {
    const c = combo(b);
    const prev = seen.get(c);
    if (prev && prev !== b.cmd) {
      throw new Error(`${c} is bound to both ${prev} and ${b.cmd}`);
    }
    seen.set(c, b.cmd);
  }

  const byCmd = new Map<string, string[]>();
  for (const b of bindings) {
    const list = byCmd.get(b.cmd) ?? [];
    const c = combo(b);
    if (!list.includes(c)) {
      list.push(c);
    }
    byCmd.set(b.cmd, list);
  }

  const grouped = new Set(groups.flatMap(([, cmds]) => cmds));
  for (const cmd of byCmd.keys()) {
    if (!grouped.has(cmd)) {
      throw new Error(`${cmd} is bound but is in no group`);
    }
  }

  const out: string[] = [];
  out.push("# Keyboard shortcuts");
  out.push("");
  out.push(
    "Default shortcuts of SumatraPDF on Windows and Linux. Generated by `bun cmd/gen-shortcuts-doc.ts` from `src/shared/Accelerators.cpp`: don't edit by hand.",
  );
  out.push("");
  out.push(
    "Any of them can be changed, see the custom shortcuts documentation. A letter without `Shift` is the lowercase key.",
  );
  for (const [title, cmds] of groups) {
    const rows = cmds.filter((c) => byCmd.has(c));
    if (rows.length === 0) {
      continue;
    }
    out.push("");
    out.push(`## ${title}`);
    out.push("");
    out.push("| Action | Shortcut |");
    out.push("| ------ | -------- |");
    for (const cmd of rows) {
      const name = names.get(cmd) ?? cmd;
      const keys = byCmd
        .get(cmd)!
        .map((k) => `\`${k}\``)
        .join(", ");
      out.push(`| ${name} | ${keys} |`);
    }
  }

  out.push("");
  out.push("## Command palette prefixes");
  out.push("");
  out.push("Typed in the document, they open the command palette in a mode:");
  out.push("");
  out.push("| Key | Mode |");
  out.push("| --- | ---- |");
  const prefixes: [string, string][] = [
    [">", "Commands"],
    ["@", "Tabs"],
    ["#", "History"],
    ["$", "Favorites"],
    ["%", "Table of contents"],
    ["&", "Thumbnails"],
    ["=", "Quick edit settings"],
  ];
  for (const [k, v] of prefixes) {
    out.push(`| \`${k}\` | ${v} |`);
  }

  out.push("");
  out.push("## Mouse");
  out.push("");
  out.push("| Action | Mouse |");
  out.push("| ------ | ----- |");
  out.push("| Zoom | `Ctrl+Wheel` |");
  out.push("| Scroll sideways | `Shift+Wheel` |");
  out.push("| Read Aloud from the clicked point | `Ctrl+Alt+Click` |");
  out.push("| Previous / next sentence while reading | `Ctrl+Alt+Wheel` up / down |");
  out.push("");

  writeFileSync(outPath, out.join("\n"));
  Bun.spawnSync(["bunx", "prettier", "--write", outPath], { cwd: root });
  console.log(`wrote ${outPath}: ${bindings.length} bindings, ${byCmd.size} commands`);
}

main();
