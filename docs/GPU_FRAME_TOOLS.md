# GP0 frame tools — who drew this pixel?

A set of tools that answer one question about a rendering bug: **which guest
function issued this primitive, and with which blend mode?**

```
tools/psx_gpu_frame.py       transport + GP0 decode + attribution (the library)
tools/gpu_frame_capture.py   capture a frame from a running game
tools/gpu_frame_layers.py    render one image per guest function
tools/gpu_frame_diff.py      diff a good frame against a bad one
tools/gpu_parity.py          frame-locked image parity vs DuckStation
tools/tests/test_gpu_frame.py
```

RetComM Studio's **Frames** tab is a viewer and launcher over exactly these
artifacts. It never decodes a GP0 packet itself, so a headless capture and the
GUI can never disagree about what a frame contained.

---

## Why this exists

`gpu_frame_dump` on the debug server has always stamped every GP0 packet with
the guest code that issued it — `func`, `pc`, `ra`, plus the linked-list OT rank
(`handle_gpu_frame_dump` in `runtime/src/debug_server.c`, `GpuGp0RingEntry` in
`runtime/include/gpu.h`). Nothing consumed that attribution, so "the glow is
opaque and the vignette is missing" stayed a description of a screenshot rather
than a pointer at a function.

---

## The provenance rule

Everything these tools report is **observed** from one execution of one frame,
and is labelled as such. It is never merged into a static claim.

A `func` in a frame dump proves that code ran and issued a primitive. It says
nothing about the call graph — the analyser's static coverage gap
(`FUNCTION_DISCOVERY.md`) stays exactly as wide as it was. Studio shows observed
counts and static names in different columns for the same reason.

---

## What is decoded faithfully, and what is not

**Faithful:**

* Vertices, including the 11-bit signed coordinate and the running GP0(E5) draw
  offset — the same offset `gp0_exec_*` applies in `gpu.c`.
* Packet word layouts for every polygon, line, rectangle, fill and copy form.
* The texpage latch. A textured polygon's own tpage word overwrites draw-mode
  state (`set_tpage_from_poly` in `gpu.c`), and later untextured polygons and
  rectangles — which carry no tpage word — inherit it. Getting this wrong
  mislabels precisely the semi-transparency mode you are usually chasing.
* All four semi-transparency blends, when layers are rendered:
  `0.5B+0.5F`, `B+F`, `B-F`, `B+0.25F` (GPUSTAT bits 5-6).

**Not:**

* Textures are never sampled. Texture pages, CLUTs and UVs are decoded and
  reported, so a wrong-CLUT hypothesis is testable, but a textured primitive
  renders as its command colour. Re-implementing texture sampling would buy
  fidelity the question does not need and add a whole new way to be wrong.
* The ring truncates each packet to `GPU_GP0_RING_MAX_WORDS` (12). Longer
  packets are decoded as far as recorded and flagged `truncated` — never
  guessed at.

---

## Transport

**One request per connection.** `io_thread_main()` in `debug_server.c` accepts,
reads a single line, replies, and closes. A client that holds the socket open
and sends a second command gets silence and then EOF, which is indistinguishable
from a hung emulator. `psx_gpu_frame.DebugConn` opens a socket per command;
`tools/debug_client.py`'s `query()` documents the same contract.

---

## Typical session

```bash
cd <your recomp project>
P=psxrecomp/tools

# 1. Put the game on the frame you want, then capture it twice: once while it
#    still looks right, once after it breaks.
python3 $P/gpu_frame_capture.py --tag good --out analysis/frames --summary
python3 $P/gpu_frame_capture.py --tag bad  --out analysis/frames --summary

# 2. What changed?
python3 $P/gpu_frame_diff.py analysis/frames/good.json analysis/frames/bad.json

# 3. Look at the bad frame one function at a time.
python3 $P/gpu_frame_layers.py analysis/frames/bad.json --out analysis/frames/bad-layers

# 4. Did the guest even run the same? (needs a patched DuckStation on 4371)
python3 $P/gpu_parity.py --frame 41230 --out analysis/frames/parity
```

`--run-to N` drives the runtime to a frame before capturing, so a savestate plus
a fixed frame number makes the whole thing reproducible.

### Artifacts

| File | Contents |
|---|---|
| `<tag>.json` | the full dump: every decoded primitive, with `func`/`pc`/`ra`/`ot` |
| `<tag>.summary.json` | totals, opcode + blend-mode histograms, per-function attribution |
| `<tag>.opcodes.json` | the server's own `gpu_opcodes` histogram |
| `<tag>.png` | the presented frame |
| `<tag>-layers/composite.png` | every primitive, in issue order |
| `<tag>-layers/layer-<func>.png` | one function's contribution, RGBA, alpha = coverage |
| `<tag>-layers/sheet.png` | labelled contact sheet of all layers |
| `<tag>-layers/layers.json` | the layer index: per-function stats and file names |
| `diff.json` | machine-readable diff, `--json` |

A busy frame's dump runs to tens of megabytes. Anything that only wants to know
*who drew what* should read `<tag>.summary.json` instead — which is what Studio
does.

### Layers render over grey, on purpose

An isolated layer is drawn over neutral grey (`--layer-backdrop`, default 128),
not black. A `B-F` subtractive vignette against black is black, and an additive
glow against black is just the glow: both blends go invisible in exactly the
view meant to show them. The composite still starts from black, as the GPU does.

---

## Reading a diff

The report leads with the two findings that name a bug on their own:

* **a blend mode that disappeared** — if `B-F` drew ten primitives in the good
  frame and none in the bad one, whatever layer used that blend (a vignette, a
  shadow) is no longer being drawn at all;
* **a function that stopped drawing** — it stopped running, or stopped reaching
  its emit path.

Then, per function: `stopped drawing`, `lost semi-transparency`,
`started drawing`, `gained semi-transparency`, `count changed`.

The diff is over **counts and flags per (function, opcode)**, never over exact
geometry. An animating effect legitimately moves its vertices every frame;
geometry appears only as a sample, for context.

---

## Turning an address into a name

Every function the tools report is an address you can name:

```bash
python3 -m project_studio analyze set-symbol --root . --pc 0x8004ABCD \
    --name LandEffect_DrawRays --status guessed \
    --note "observed issuing GP0 primitives (frame capture)"
python3 tools/sync_symbols.py     # regenerates psx_symbols.h / PSX_FN_*
```

Studio's Frames tab has a **Save name** box that calls exactly this, so
`symbols.toml` keeps a single writer.

---

## Parity scope

`gpu_parity.py` compares **images**, not GP0 streams. The DuckStation oracle
patch (`tools/duckstation/psxrecomp_oracle.patch`) implements `screenshot`,
`read_vram`, `gpu_state`, `run_to_frame`, `step` and `pause` — but not
`gpu_frame_dump`, so there is no packet stream to compare on that side. Adding
one to the patch is the obvious next step if image parity keeps saying "the
streams must differ" without saying where; until then, `tools/cosim.py` brackets
the divergence.

Both instances must be driven identically — same disc, same starting state.
Parity between two runs that were not driven the same way means nothing.

---

## Tests

```bash
python3 -m unittest discover -s tools/tests -p 'test_gpu_frame.py'
```

Covers the packet word layouts against `gpu.c`'s command lengths, the signed
vertex, the E5 offset, the textured-polygon tpage latch, the four blends,
truncation flagging, the attribution rollup, the diff verdicts, and a stub
server that reproduces the one-request-per-connection contract.
