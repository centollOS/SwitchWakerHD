# Issue #100: every Quest Log shows the current date

## Cause

The game stamps a save with `OSGetTime` (SaveMgr 02721A8C, Quest Log data +0x18). The file select list converts
that stamp with `OSTicksToCalendarTime`. Our `OSGetTime` returned the time since the start, and
`OSTicksToCalendarTime` ignored its ticks and returned the host's current time. So every Quest Log showed "now".

## Fix: upstream's #322, cherry-picked

- 2dd617a3 (here 434f3da2): `OSGetTime` is the calendar clock (guest timebase + a boot bias), and
  `OSTicksToCalendarTime` converts its ticks (`hle/os_time.{h,cpp}`). Quest Logs saved by earlier builds (a
  time-since-start stamp) get the file's last-modified date when `cking.sav` is read.
- 04e1618c (here 14833564): a fixed date under `WWHD_TEST_FIXED_SEED`, a `WWHD_OS_TIME` override, and the bias
  kept in full save states (section `OSTB`). Conflict in `savestate.cpp`: upstream's `kSecAspectParticles`
  section (a feature we do not have) was left out, only the `OSTB` lines taken.

## Results (2026-11-01, console, HOS 22.1.0, handheld, USA)

- `os_time_test` passes (Mac).
- Boot log: `[time] calendar clock (OSGetTime): 2026-11-01 21:14:49 (host clock)`. Quest Logs 1 and 2 of the
  console's save had 3.9 and 30.4 minutes-since-start stamps: both repaired, and the list showed
  28/10/2026 16:34 (the file's date) instead of the current time.
- Saved Quest Log 1 in game: `cking.sav` +0x18 holds 2026-11-01 21:20:10. After a restart the list shows
  01/11/2026 21:20 for it and 28/10/2026 16:34 for Quest Log 2 (written back by that save). No repair lines then.
- The console's save and NRO were restored afterwards.
