# Changelog

## 1.1.6

- Follows the program language (ten languages).

## 1.1.5

- "Reveal the whole map" works again: the game had moved the map data it changes, so the plugin no longer found where to apply it.

## 1.1.4

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.

## 1.1.3

- Rebuilt with the updated plugin SDK headers (POEFixer 365 adds optional calls for UI control names, stash objects and tabs, wider entity scans and the host automation flag). No change in what the plugin does.

## 1.1.2

- The "?" next to a section title now sits in its own gap instead of on the section's line.
- Diagnostics now looks the same as in the other POEFixer plugins.
- The bottom of the settings tab names the plugin's original author, as in the other plugins maintained by POEFixer.

## 1.1.1

- First POEFixer release of Get Back My Zoom.
- Safer patching: a change is written only where its pattern is found exactly once and the original bytes are confirmed, and the game is opened with the minimum access needed.
- Changes left behind by a crash are undone automatically the next time POEFixer connects to the same game.
- Disabling the plugin now really keeps it off: it no longer re-applies its changes when the game is attached again.
- Restarting the game or switching to another game client is handled: changes are undone in the old process and applied to the new one.
- No more stutter: the game scan and all writes run in the background instead of on a frame.
- Redesigned settings tab with a clear state per feature, a "Scan again" button, a logarithmic light radius slider with "Game value", and a diagnostics card.

## 1.1.0

- Original release by Ömer Faruk Arpa.
