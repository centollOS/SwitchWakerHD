# Reporting a bug

A good report lets us reproduce the problem on our own console. Three kinds of files make the difference:

| File | What it is | Where it is on the SD card |
|---|---|---|
| **Session log** | What the game did, written while you play (one per session, the 10 newest are kept) | `sdmc:/switch/wwhd/logs/wwhd_<date>_<time>.log` |
| **Save state** | Link's place, hearts, items and progress, a few KB (no game data) | `sdmc:/switch/wwhd/states/slotN.wwstate` |
| **Save file** | Your Quest Logs | `sdmc:/switch/wwhd/save/user/cking.sav` |

If the console showed *"The software was closed because an error occurred"*, also add the newest file from
`sdmc:/atmosphere/crash_reports/`.

## 1. Note the version and what you did

- The version is in the options menu's **Language / About** tab (hold **Minus** while playing).
- Write down where you were (island or dungeon, which room), what you did just before, and whether you had
  changed anything in the options menu (Picture tab: resolution, effects; sys-clk clocks).

## 2. Make a save state where the problem happens

1. Go to the place, just before the problem if you can.
2. Hold **Minus** for half a second, open the **Saves** tab and press **Save** on a slot. You can save whenever
   you control Link (not during a cutscene, a dialogue or a scene change; a notice tells you when it has to wait).
3. The Saves tab also lists the exact files to attach under *"For a bug report, attach these files"*.

We load your state on our console from within a Quest Log, so your save file comes along with it.

Don't attach **full** save states (`.bin`, the "Full save states" option): they contain game data.

## 3. Keep the log of the session that went wrong

The newest log in `logs/` is the current session. Every start of the game makes a new one and only the 10 newest
are kept, so **copy the logs before you start the game many more times**. If you aren't sure which one it is,
attach the two or three newest: their names hold the date and time the session started.

## 4. Copy the files to a computer

Any of these works:

- **Take the SD card out** and put it in the computer (with the console off).
- **Over USB with hekate:** reboot into hekate, then *Tools → USB Tools → SD Card*, and connect the cable. The SD card
  shows up as a drive on the computer. Eject it before you unplug.
- **Over Wi-Fi or MTP** with a homebrew FTP server or file manager, if you already use one.

## 5. Open the issue

Open an issue at [github.com/centollOS/SwitchWakerHD/issues](https://github.com/centollOS/SwitchWakerHD/issues/new/choose)
and choose **In-game bug** (or *Setup, start or other bug* for installing, building or a game that doesn't start). Zip the files and drag the zip
into the issue form, together with the version and your notes. Screenshots or a short video (the console's
Capture button) help too.

If you can't add some of the files, say why in the form; a report with only a log is still useful.
