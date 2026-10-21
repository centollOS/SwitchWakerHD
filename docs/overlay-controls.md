# Settings overlay controls

Press Esc (or F1) to open Settings. With a captured mouse, the first Esc frees the mouse and a
second one opens Settings. On a controller, press Home or hold Select / Minus.
On macOS, Cmd+, (or **Settings…** in the app menu) also opens and closes Settings. This
shortcut is fixed: it is not listed in Shortcuts, and it still opens Settings if you bind Cmd+,
to another action.
Use the D-pad or left stick to navigate, A to select, B to go back or close,
and L / R to switch tabs.

Mouse movement, clicking, wheel scrolling, and dragging the scroll bar take over
from controller navigation, including when the controller is centered. A stick or
trigger already held when you use the mouse will not keep moving the menu. Release it and make a new controller input
to return to controller navigation. The right stick does not navigate the overlay.
Keyboard navigation remains available while using the mouse.

## Quitting the game

**Quit Game** sits at the bottom right of Settings, on every tab (on Android next to
**Close Settings**). It is meant for a Steam Deck in Gaming Mode, a handheld or a TV
setup where there is no window to close. With a controller, press Down until the
button is highlighted and press A.

It first asks *Quit Wind Waker HD?*. **Cancel** is selected first, so pressing A
straight away (or B) returns to Settings and the game keeps running. **Quit** closes
the game the same way as closing its window. Settings are already saved as you change
them. Progress since your last in-game save is lost, so save in the game first (or
use a save state). On Android the game also leaves the recent apps list.

## Changing bindings

**Controls** changes the game's inputs. **Shortcuts** changes Settings, full screen,
screenshots, save/load slots, graphics, picture-in-picture, the performance overlay and audio.
Each shortcut has a key, an alternate key and a controller binding. **Settings** uses Esc, with
F1 as the alternate key and Home on a controller; all three can be changed there. Holding Select also
opens Settings unless Select is used as a combination's hold button.

Select a key field and press a key or mouse button. Select a controller field and tap a button,
or hold one button and press another. While a field waits for input, that input is recorded:
Escape does not close Settings, L/R does not switch tabs, and Select/Home does not open or close
Settings. Outside capture, Escape still closes Settings. With a captured mouse, the first Escape
only frees the mouse, also after you move Settings to another key.

Right-click a binding to clear it, in either tab. With a binding focused, Delete, Backspace,
or the controller button mapped to X clears it too. Right-click while recording a mouse
binding records the right mouse button instead. Settings must keep a working binding or its
held-Select shortcut. Changes are saved automatically and survive restarting the game.

Capture/Share, rear paddles and touchpad clicks are available when your controller supplies
them. They start unbound. A combination consumes only its two buttons while both are held;
a single-button shortcut consumes that button while held.

## Diagnostics

If asked to capture a menu problem, `WWHD_OVERLAY_TRACE=1` records controller
axes (raw and normalized, including triggers), held buttons, menu input events,
focus/navigation and scroll changes in `captures/wwhd.log` while Settings is open.
It enables the normal log file for that run (unless `WWHD_LOG_FILE=0` is set).
Unchanged input is omitted; very noisy input is rate limited with a notice, and
trace output stops after about 512 KiB. Restart to capture another session.

`WWHD_OVERLAY_MOUSE_HANDOFF=0` temporarily disables mouse ownership of controller
navigation, so the original scrolling behavior can be tested. Normally leave it
unset: mouse handoff stays enabled by default.

On Windows, put a file named `menu-diagnostics.bat` beside `Wind Waker HD.exe` (the program you
normally start), containing:

```bat
@echo off
cd /d "%~dp0"
set "WWHD_OVERLAY_TRACE=1"
set "WWHD_OVERLAY_MOUSE_HANDOFF=0"
"Wind Waker HD.exe"
```

Double-click it to launch the game. Use the stick once and release it, open Settings with Esc or F1, click
an option, then scroll down with the mouse wheel. Wait a few seconds, quit, and attach
`captures\wwhd.log` from the game's data folder to the issue. Keep the session around two minutes or
less. The variables apply only to this launch; starting `Wind Waker HD.exe` normally restores the
defaults.
