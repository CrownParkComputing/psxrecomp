# Deterministic input-route runner

`tools/debug_client.py input_route` replays a JSON route through the native
debug server's guest-VBlank input queue. It is intended for reproducible boot,
menu, and gameplay evidence where a host-side press or a wall-clock delay would
make pad edges depend on scheduling.

Run it against a debug-tools runtime with:

```text
python tools/debug_client.py --port 4370 --route-timeout 30 input_route route.json
```

`--route-timeout` is a completion deadline in seconds. It bounds only the wait
after `input_route_start`; route loading and upload still use the debug client's
normal socket timeout. The runner does not call `sleep`: durations are consumed
by the runtime once per guest input sample/VBlank.

## Route format

The root object must use format `psxrecomp-input-route-v1` and contain a
non-empty `steps` array:

```json
{
  "format": "psxrecomp-input-route-v1",
  "steps": [
    {"pad_word": "0xFFFF", "frames": 120},
    {"pad_word": "0xBFFF", "frames": 1},
    {"pad_word": "0xFFFF", "frames": 30}
  ]
}
```

`pad_word` is the raw 16-bit PS1 pad word: it must be in `0x0000` through
`0xFFFF`, and is active-low (`0` means pressed). `0xFFFF` is all released.
`frames` is a positive guest input-sample/VBlank count, not milliseconds. A
route has at most 4096 steps, matching the server queue; each duration must fit
the server's positive signed integer field.

The loader normalizes numeric and string integer spellings before upload. The
receipt's `route_digest` is `sha256:` plus SHA-256 over compact, sorted-key JSON
for this semantic object:

```json
{
  "format": "psxrecomp-input-route-v1",
  "steps": [
    {"frames": 120, "pad_word": "0xFFFF"}
  ]
}
```

Whitespace, object-key order, and equivalent integer spellings therefore do
not change the digest. Descriptive extra JSON fields are not part of the
semantic digest.

## Protocol and completion

The native server closes each TCP connection after one response. The runner
reuses its initial connection once, reconnects for every remaining command,
and checks every response in this order:

1. `input_route_clear`
2. one `input_route_append` per step (`buttons` receives the normalized raw
   pad word and `frames` receives the duration)
3. `input_route_start`
4. repeated `input_route_status` requests until the server reports
   `active=false`, `index=step_count`, and `remaining=0`
5. `frame`, to capture the end-frame receipt

The start response supplies `start.frame`; the final status and frame response
are retained under `end`. A stopped route or malformed progress is an error,
not completion. If the deadline expires, the runner makes a bounded best-effort
`input_route_stop` and exits with a nonzero status; it does not emit a success
receipt.

On success, stdout contains one compact JSON receipt. It includes
`route_digest`, `step_count`, `frame_count`, `start`, `end`, and
`completion: "exact"`, for example:

```json
{"completion":"exact","end":{"frame":151,"status":{"active":false,"index":3,"remaining":0,"steps":3}},"frame_count":151,"ok":true,"route_digest":"sha256:...","route_format":"psxrecomp-input-route-v1","start":{"frame":0,"server":{"ok":true,"start_frame":0,"steps":3}},"step_count":3}
```

The frame values in this example are illustrative; the route digest and the
server's start/end values are the evidence to retain with a run.

## Hermetic checks

The parser, digest normalization, upload order, exact completion predicate,
and timeout path can be tested without a runtime:

```text
python -m unittest tools/tests/test_debug_client_input_route.py
```
