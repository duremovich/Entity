# Integration Tests

Headless, scriptable integration tests for the media server. Each test is a JSON
script that drives the app through a scenario and asserts on output via pixel
hashes.

## How a test runs

```bash
# Windows: double-backslashes in paths as needed
./build/bin/Release/EntityMediaEditor.exe --headless --script scripts/integration/smoke.json
```

The app:
1. Starts with a hidden window (`--headless`).
2. Executes the script's commands.
3. On `CaptureHash`: reads a compose target's pixels, computes FNV-1a 64-bit hash,
   writes `<hex> <WxH>\n` to the specified path, and optionally compares against
   a golden file.
4. Exits on `Exit` command. Non-zero exit code if any `CaptureHash` failed.

## Script conventions

- Test scripts live in `scripts/integration/`.
- Hash outputs go to `test_output/<test_name>/` (gitignored).
- Golden hashes live in `tests/goldens/<test_name>/` (committed).
- A test script is the source of truth for the scenario — scenario comments
  belong in the `description` field.

## CaptureHash command

```json
{
    "type": "CaptureHash",
    "hashFilepath": "test_output/smoke/frame_010.hash",
    "goldenFilepath": "tests/goldens/smoke/frame_010.hash",
    "composeSlot": 0
}
```

- `hashFilepath`: where to write the computed hash. Always written.
- `goldenFilepath`: optional. If given, the command fails on mismatch.
- `composeSlot`: which compose target (screen) to hash. Defaults to 0.

Record a new golden by running the test without a `goldenFilepath` set, then
copy the `hashFilepath` output into `tests/goldens/`.

## Phase A status

MVP scaffolding only. Still to add in future sessions:
- A golden/record workflow script
- CTest wiring so `ctest --output-on-failure` drives these
- Goldens checked in for seek, mixed-fps, multi-screen, split/duplicate+keyframes,
  ping-pong, project round-trip, and blend-mode tests

See `docs/status/CURRENT.md` Phase A task 5 for the full plan.
