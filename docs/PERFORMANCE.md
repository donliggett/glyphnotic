# Performance

What Glyphnotic costs per frame, measured in Phase 5 (2026-11-11) with
Glyphnotic 1.0.0.

**Test machine:** Windows 11 Pro 25H2, NVIDIA GeForce RTX 4070 Ti (driver
621.01), OBS Studio 33.0.1, Direct3D 11 renderer, 1920×1080 canvas at 60 fps.

## Summary

- An Ink look on ASCII costs about **1.3 ms** of GPU time per frame on a
  1920×1080 source; Mosaic, Poster and Console about **1.2 ms**; the boiling
  looks Static and Fever 1.5–1.7 ms; **Every Glyph about 4.0 ms**. A 60 fps
  frame lasts 16.7 ms.
- In OBS at 1920×1080, 60 fps, recording with NVENC for 60 s: three Ink/ASCII
  instances on three 1080p sources, one Every Glyph instance, and three Fever
  instances each ran with **0 lagged and 0 skipped frames**.
- Larger cells are cheaper: at cell scale 2× Ink Black costs 0.9 ms and Every
  Glyph 1.5 ms.
- About four Every Glyph instances at 1080p fill a 60 fps frame on this GPU
  (15.6 ms; 16 of 600 frames lagged); three run cleanly.
- The CPU cost is small: about 0.05 ms per filter per frame to issue its
  passes on Direct3D 11 (0.6 ms on OpenGL).

## Method

GPU time comes from OBS's own source profiler, which times each source's
rendering with libobs GPU timer queries (`gs_timer`, `gs_timer_range`).
`tools/reference/run_performance.py` runs the filter in the test harness (a
headless libobs with the real renderer; `tools/README.md`) on a 1920×1080
still (`spring.png` scaled to cover 1080p) at 60 fps for 600 frames per
case, and reads the profiler's averages.

A filter timed alone on an otherwise idle GPU reads 1.3–2× its cost and
varies from run to run (Ink Black alone measured 1.8, 2.4 and 3.0 ms in
three runs), likely because a short burst of work on an idle GPU runs below
full clocks. Timed as several instances in one scene the figure is steady,
and the scene's time grows linearly with the count (Ink Black: 3.95 ms for 3,
4.95 ms for 4, 9.95 ms for 8), so **each case is timed as three instances,
each on its own 1080p image, and the GPU time per instance is the scene's
time divided by 3**. The "alone" column is one instance by itself. Every
figure includes drawing the image under the filter, which costs about
0.02 ms.

## GPU time per look (Direct3D 11, 1920×1080)

| Filter | Look | GPU per instance (ms) | One instance alone (ms) |
| --- | --- | --- | --- |
| Ink | Black | 1.32 | 2.36 |
| Ink | Tint | 1.27 | 2.41 |
| Ink | Phosphor | 1.30 | 2.41 |
| Ink | Amber | 1.25 | 2.29 |
| Ink | Every Glyph | 4.01 | 4.13 |
| Ink | Static | 1.51 | 2.70 |
| Ink | Fever | 1.66 | 2.77 |
| Mosaic | Blocks | 1.16 | 1.80 |
| Poster | Poster | 1.17 | 2.09 |
| Console | Console | 1.17 | 2.14 |

Three instances of every look ran without a lagged frame.

### Cell scale

| Look | 1× | 2× | 3× | 4× |
| --- | --- | --- | --- | --- |
| Ink Black | 1.30 | 0.92 | 0.85 | 0.78 |
| Ink Every Glyph | 3.91 | 1.47 | 1.50 | 1.52 |

(GPU ms per instance.) At 1× the score pass, which loops over every glyph
for every cell, dominates Every Glyph's time; at 2× there are a quarter as
many cells and the passes that run at the source's full resolution (Soften,
Sample, Draw) dominate, so larger scales save little more.

### Character set (Ink, Black's other settings)

| Set | Glyphs | GPU per instance (ms) |
| --- | --- | --- |
| Blocks | 5 | 1.11 |
| Lines | 25 | 0.99 |
| ASCII | 95 | 1.28 |
| Every glyph | 503 | 3.91 |

### Instances

| Look | Instances | Scene GPU (ms) | Lagged frames (of 600) |
| --- | --- | --- | --- |
| Ink Black | 1 | 2.40 | 0 |
| Ink Black | 3 | 3.95 | 0 |
| Ink Black | 8 | 9.95 | 0 |
| Ink Every Glyph | 1 | 3.98 | 0 |
| Ink Every Glyph | 3 | 11.80 | 0 |
| Ink Every Glyph | 4 | 15.57 | 16 |
| Ink Every Glyph | 8 | 31.34 | 298 |

### OpenGL

OBS's source profiler reported 0 GPU time on OBS 33's OpenGL renderer on
Windows, so there are no OpenGL GPU times. Frames lagged in the same runs:
every look ran three instances at 60 fps without a lagged frame except
Every Glyph, which lagged 88–97 of 600 frames with three instances and none
with two; eight Ink Black instances ran cleanly. Issuing the passes costs
about 0.6 ms of CPU per filter per frame on OpenGL against 0.05 ms on
Direct3D 11. (Linux uses OpenGL; on Linux the check ran on a software
renderer, so it has no performance figures.)

## In OBS

`tools/obs-tests/run_obs_tests.py` (`tools/README.md`): OBS itself with the
test profile set to a 1920×1080 base and output canvas at 60 fps, recording
to a file with NVENC (simple output mode, 6000 kbps) for 60 s after 10 s on
the scene. Lagged frames are OBS's "frames missed due to rendering lag",
skipped frames its "skipped frames due to encoding lag" (the Stats window's
figures, from OBS's log at the end of the recording). The render time is
OBS's "average time to render frame" (the CPU side of OBS's render loop),
sampled every frame.

| Scene | Frames drawn | Lagged | Skipped | Average render time (ms) |
| --- | --- | --- | --- | --- |
| Three 1080p images, no filter | 3615 | 0 | 0 | 0.254 |
| Three 1080p images with Ink Black, Tint and Phosphor | 3616 | 0 | 0 | 0.369 |
| One 1080p image with Ink Every Glyph | 3614 | 0 | 0 | 0.313 |
| Three 1080p images with Ink Fever | 3615 | 0 | 0 | 0.368 |

The images are shown at half size on the canvas; each filter works at its
source's full 1920×1080. OBS's own frame-time figure counts only the CPU's
part, so it rises by about 0.04 ms per filter; the GPU's part is in the
tables above.
