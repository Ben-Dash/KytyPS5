# Public developer references for PS5 and Yōtei

Verified2026-10-14; these are public primary sources, not access to the private SDK
or Sucker Punch's game source. No partner account was created/accessed.

- [Sony: Showing your Game to PlayStation](https://sonyinteractive.com/en/news/blog/showing-your-game-to-playstation/)
  describes approved registration and the Global Developer and Publisher Agreement
  before access to development tools and documentation. The platform SDK describes
  platform programming; it does not supply the source of this game's renderer.
- [Sucker Punch: Ghost of Yōtei tech deep dive](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/)
  describes compute-based visibility, allocation and draw-record generation followed
  by CPU readback and assembly of frame command lists. This supports keeping real
  producer-to-reader dependencies when optimizing the emulator's readback path.
  It also describes expanded scene instances,16-bit fog work and dynamic resolution;
  output1920x1080 is not proof of the game's internal target dimensions.
- [Eric Wohllaib / Sucker Punch: Ambient Diffuse Lighting in Ghost of Yōtei, GDC2026](https://gdcvault.com/play/1035740/Ambient-Diffuse-Lighting-in-Ghost)
  explains ambient probes, ray-tracing data/material access and screen-space probe GI.
  The session page was read; the31.6MB slides exceeded the web reader's fetch limit.
  Detailed slide algorithms/timings have not yet been reviewed.

For the current four bounded experiments, use the public engine description as
context. Expected readback visibility, synchronization and ownership still come
from guest/API and native contracts, checked with synthetic independent fixtures.
SDK samples or a lighting talk alone cannot prove our exact performance culprit.

The [official PS5 open-source catalogue](https://www.playstation.com/en-us/oss/ps5/)
lists components such as Eigen, Cairo and WebKit; that page supplies neither a
renderer SDK sample nor Yotei source. A full public PS5 renderer was not found
in the official sources checked; this is a bounded search result, not proof
that no public sample exists anywhere. The
[AMD RDNA2 ISA guide announcement](https://gpuopen.com/news/rdna2-isa-available/)
is useful for shared shader semantics, not reconstruction of the game's frame graph.

The2026-10-15 [GitHub research and measured function/GPU follow-up](yotei-render-research-and-profile-2026-10-15.md)
records concrete AnyPS5/SharpEmu implementations, what already exists locally,
four native measurement runs and the remaining limits. Main CPU polling is
measured separately from CPU function work and GPU pass time.
