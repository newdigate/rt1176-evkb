# SynthUI repo — design

Date: 2026-08-15. Status: approved in session, pending implementation.

## 1. What this creates

A new sibling library repo at `~/Development/SynthUI` — local git only, **no
remote** — seeded with the DC component reference set and the recovered ReBirth
material, curated into two provenance areas. No widget code yet. This is the
future home of the LVGL control-surface widgets (knob, fader, lamps, LED
buttons, level meter, piano keys, seven-segment, toggles) for the synth work
(acid bass voice, transport, step sequencer).

## 2. Why a new sibling repo

- Everything firmware imports lives as its own repo under `$TEENSY_LIB_ROOT`
  (default `~/Development/<lib>`); the `LVGL` sibling is the direct precedent —
  own repo, `library.properties`, imported by a dedicated macro, linked as a
  plain CMake static lib.
- The reference material that defines what to build (the `.dc.html` set, the
  ReBirth guide) currently sits unversioned in `~/Development/components`,
  which is also a live editor working directory full of paste droppings.
  A curated copy versions the design truth next to the future implementation
  without entangling the editor workspace.

## 3. Facts established during design (verified, not assumed)

- The Sheet pages load their sprite art by **bare relative path**
  (`src="crop-fader.png"`; one sheet uses `src="uploads/strip.png"`), so the
  browser-viewable set only works if its flat layout is preserved.
- `support.js` is a **generated artifact of the user's own `dc-runtime` tool**
  (header: "GENERATED from dc-runtime/src/*.ts … rebuild with `bun run
  build`") — clean provenance, coverable by the repo's MIT license.
- The `rebirth/` material is recovered from the 1997–2000 ReBirth mod archive
  (single mod `mellow.rbm`); community mod art, **rights unclear** — hence the
  quarantine rules in §5.
- The nine leaf components are procedural vector (SVG template +
  `renderVals()` logic), authored to VGLite-shaped constraints ("linear
  gradients only", matrix rotation, "no sprite atlas"). The current LVGL 9.4
  stack in this tree renders **software-only** (`LV_USE_DRAW_SW 1`; VGLite
  driver pruned on vendoring for the license firewall). Note recorded during
  this design: the RT1176 **does** have the GC355 VGLite GPU (RM GPU2D
  chapter, 0x4180_0000, IRQ 60) — the 2026-07-27 LVGL spec's "no VGLite GPU"
  claim is wrong; a correction task exists. Not using it remains correct
  (license firewall + no GC355 model in qemu2, so no QEMU gate could cover it).

## 4. Layout and exact contents

```
~/Development/SynthUI/            new git repo, branch master, NO remote
├── LICENSE                       MIT
├── README.md                     what this is + provenance rules (§5)
├── library.properties            name=SynthUI, category=Display, modeled on LVGL's
├── .gitignore                    build/, .DS_Store
└── reference/
    ├── dc/                       flat, relative paths preserved:
    │   │                           9 leaf components: Fader, Knob, Lamp, LedButton,
    │   │                           LevelMeter, PanelButton, PianoKey, SevenSegment,
    │   │                           SlideToggle (.dc.html)
    │   │                           8 sheets: Display, Fader, Keyboard, Knob, Meter,
    │   │                           Panel, Step Buttons, Toggle (" Sheet.dc.html")
    │   │                           support.js
    │   │                           10 sprite PNGs: c-lamps, c-switches, c-transport,
    │   │                           crop-909, crop-909-strip, crop-fader, crop-keys,
    │   │                           crop-led, crop-meter, crop-toggle
    │   └── uploads/strip.png     kept at this path (a sheet references it)
    └── rebirth/                  rebirth-component-guide.md,
                                  rebirth-components.png, rebirth-palette.png
```

Excluded deliberately: `uploads/pasted-*.png` (editor paste droppings),
`.thumbnail`. `src/` is deliberately absent until the first widget (the Knob
pilot) lands — no placeholder code.

## 5. Provenance rules (the README contract)

- `reference/` is design reference, **never compiled**.
- Nothing under `reference/` may be converted into C arrays, fonts, or sprite
  data in `src/` without an explicit rights decision recorded in the README
  first. This bites hardest for `reference/rebirth/` (recovered mod art).
- Clean-room vector rebuilds from the guide's *written descriptions* are the
  intended path — same firewall discipline as the evkb tree's license audit.
- The repo's MIT LICENSE covers its own content: the DC component set,
  `support.js`, and all future `src/` code.

## 6. Git details

- `git init -b master`, one initial commit containing everything in §4.
- No remote configured ("just locally for the moment").
- `library.properties` mirrors the LVGL sibling's fields; `url=` names the
  intended future home (`github.com/newdigate/SynthUI`) but nothing is pushed.

## 7. Non-goals

- No GitHub remote, no push.
- No `evkb.cmake` import macro or pin — that arrives with the first consuming
  example.
- No widget code, no LVGL dependency yet.
- `~/Development/components` stays untouched as the editor's working
  directory.

## 8. Verification

- `git -C ~/Development/SynthUI log --oneline` shows exactly one commit;
  `git status` clean.
- `reference/dc/Knob Sheet.dc.html` opens in a browser and renders as it does
  from the original directory (relative art paths intact).
- `grep -r "pasted-" ~/Development/SynthUI` and a check for `.thumbnail` find
  nothing.
- The original `~/Development/components` is bit-for-bit untouched.
