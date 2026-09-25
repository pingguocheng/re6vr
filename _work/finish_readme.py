"""Finish the README repair: keep the good UTF-8 file, add the handoff section, state the loss.

Facts established by measurement (check_encoding.py, repair_readme.py):

  * the file is now valid UTF-8 with ZERO replacement runes;
  * the damage was introduced by the one PowerShell here-string insertion in this session;
    the bytes it converted are gone, so the Chinese prose of the older notes rendered as
    unreadable characters before this repair removed them;
  * everything ASCII survived untouched: every address, offset, path, command, code block and
    table, and all 99 headings' structure.

So the repair writes a file that is (a) valid UTF-8, (b) carries an English note about the loss at
the top, and (c) ends with the current handoff section, written from this session's own notes.
Nothing here pretends the lost prose is back.
"""
MARK = "# RE6 VR Mod"
p = r"C:\re6vr\README.md"

text = open(p, encoding="utf-8").read()
i = text.find(MARK)
if i < 0:
    raise SystemExit("marker not found - refusing to write")
body = text[i:]

note = """# RE6 VR Mod - handoff guide

> **README damage note (2026-09-25, honest record).** A PowerShell here-string used to insert one
> section re-encoded this whole file to the machine's ANSI code page. The UTF-8 bytes of the
> non-ASCII text were converted and are **not recoverable**; they have been replaced by `?`.
> What survived intact is everything that matters operationally: **all addresses, field offsets,
> commands, file paths, code blocks and tables**, plus the overall structure (99 headings).
> The English notes added in the same sessions are also intact. So treat the `?` runs as lost
> prose, not as data.
>
> **Current state, in one place** (the rest of this file is still the reference for everything
> built before 2026-09-25):

"""

handoff = """
## HANDOFF 2026-09-25 - start here

**What changed tonight**: the camera object is no longer hunted in memory. The engine's own
`mCameraOrg` writer is hooked, and it hands over the object address directly.

```
marker files:  re6vr_cam.txt = 1        (the memory-scan probe)
               re6vr_camhook.txt = 1    (the writer hook - the one that matters)
               re6vr_camwrite.txt      (write experiment: "sweep" or "test")

run:           python scripts\\play.py --sweep     (archives the old log, arms the sweep, launches)
```

### Established by measurement (do not re-derive)

| fact | evidence in the log |
|---|---|
| writer function = `BH6.exe + 0xFF9B0`, hook installs fine | `camhook: target 004FF9B0 ... first bytes 83 EC 10 53 56 57` |
| `this` is stable for a whole session (e.g. `1F47A060`) | every call reports the same address |
| it is NOT per-frame: 2 calls per session (all-zero at process start, real pose at level build) | `00:57:08` then `01:00:23` |
| the property offsets ARE right - the engine writes a valid look-at pose itself | `pos (1840.25 502.31 -3716.55) ... up len 1.000 fov 37.0 deg` |
| writing `+0xE40` (targetPos) has NO effect on the rendered view | write audit clean, picture static (`00:44` run) |
| never identify the object by its header | `vtable` reads `0151A394` (= vtable+0x14), `+4` = `FFFFFFFF` |
| a level takes ~3 minutes to build after launch (read save -> cutscene -> level) | hook call #2 timestamps |

### Next step (one command, one observation)

```
python scripts\\play.py --sweep
```

Load a save, **get into the level and walk around for 1-2 minutes**. The sweep cycles 5 candidate
write offsets, 6 s each (30 s per round), and the log labels every step:

```
camwrite: the engine has a camera pose - THE SWEEP STARTS NOW. 5 candidates, 6.0 s each (30 s per round).
camwrite: NOW TESTING 1/5 (0-6s):   hooked object +0xE40  (measured no effect)
camwrite: NOW TESTING 2/5 (6-12s):  hooked object +0x1050 (the copy's source for entry 0)
camwrite: NOW TESTING 3/5 (12-18s): hooked object +0x40   (sibling-title targetPos offset)
camwrite: NOW TESTING 4/5 (18-24s): source object +0x50   (what 0x4F9950 copies in - strongest)
camwrite: NOW TESTING 5/5 (24-30s): source object +0x60   (the same object's up vector)
```

* **picture swings -> note the step number**: that offset is the write point, and head tracking is
  then only "replace the swing with the headset's yaw".
* watch `[survived N frame(s), overwritten M]`: an M that keeps rising means the write lands but the
  engine rewrites that field every frame - a different problem with a different fix.
* **nothing moves in 5 steps -> the renderer reads a third copy.** Then stop writing parameters and
  go at the view matrix: `matrix_probe` already classifies the matrices the engine uploads, and the
  lesson from tonight is to use *the engine's own uploaded matrix*, never a guessed shape.

### Traps hit tonight (all fixed - do not repeat)

1. `set VAR=1` then launching through Steam does **not** pass the environment on: the experiment
   silently never ran. Write switches as **marker files**.
2. Do not decide *when* to start with a timer: the signal now is **the engine putting a valid pose
   in the object**, which is an observed transition, and the sweep cycles so timing stops mattering.
3. Each run **truncates** the previous log: `python scripts\\play.py` archives it to
   `_work\\_archive\\` first.
4. Do not edit this project's files with PowerShell `Set-Content` / here-strings - that is what
   damaged this README. Use the file tools, which write UTF-8.
"""

with open(p, "w", encoding="utf-8", newline="") as fh:
    fh.write(note + body.rstrip() + "\n" + handoff)
print("wrote README.md: %d bytes UTF-8" % len(open(p, 'rb').read()))
