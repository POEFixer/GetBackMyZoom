# Get Back My Zoom

A POEFixer plugin for **Path of Exile 2** that restores a much further camera
zoom-out, plus a few visual quality-of-life changes.

## What you get

| Feature | What it does |
|---|---|
| **Unlimited zoom out** | Removes the camera's zoom-out limit, so the mouse wheel zooms out much further. |
| **No fog on the Atlas** | Removes the fog that covers unexplored parts of the Atlas. |
| **Reveal the whole map** | Shows the full layout of the area on the map instead of only the explored parts. |
| **Larger light radius** | Makes the light around your character reach further (adjustable). |

Every feature changes a few bytes of the running game and is undone again when
you switch it off.

## Using it

1. Install **Get Back My Zoom** from POEFixer's **Plugin Manager** (the first
   tab under Plugins; it also keeps the plugin up to date), then tick
   **Enabled** at the top of the plugin's own tab.
2. Tick the features you want in the plugin's tab. They are applied to the
   running game at once and re-applied whenever the game is attached again.
3. Untick a feature to put the original back.

If the status shows **No access**, Windows refused to let POEFixer change the
game: start POEFixer as administrator.

## Settings

**Game connection** shows whether the game is attached and scanned, with a
**Scan again** button (undoes everything, reads the game code again and
re-applies what is switched on; use it after a game update).

**Camera and map / Light**: one checkbox per feature, each with its state:

| State | Meaning |
|---|---|
| Active | The change is in place in the running game. |
| Off | Found in this game version, currently off. |
| Waiting | The game is not attached or not scanned yet. |
| Not found | The game code changed (probably a game update); the feature stays off. |
| Not unique | The code was found in more than one place; nothing is written. |
| Already changed | The code is already changed by another copy or tool; restart the game. |
| Failed / Unsafe | A safety check failed; hover the state for the reason. |
| Undone by game | The game put the original back; switch the feature off and on to retry. |

**Radius** sets the light radius value (100-50000); **Game value** puts the
game's own value back.

**Diagnostics** (collapsed) lists each feature's location and original bytes
and the recent activity, with a button that copies a report for bug reports.

## Safety

- Changes are written only where a known byte pattern matches exactly once,
  after checking the bytes are still the original ones; they are read back
  after writing.
- The original bytes are saved to `config/restore_journal.json` before the first
  change. If POEFixer stops without undoing its changes, they are undone the
  next time it connects to the same game process.
- Everything is undone when you switch a feature off, disable the plugin, detach
  the game or close POEFixer. Closing the game needs nothing undone.

## Known limits

- After a game update a feature may show **Not found** until the plugin is
  updated with new patterns.
- The light radius replaces a game constant, which the game may also use for
  other lighting.
- Settings live in `config/settings.json` in the plugin folder. Settings from
  the original plugin are read as they are.

## Credits

Originally created by Ömer Faruk Arpa (https://github.com/omrfarukarpa/poefixer-getback-myzoom).
Maintained by POEFixer since October 2026.

Licensed under the GNU General Public License v3.0, see [LICENSE](LICENSE).
Copyright © 2026 Ömer Faruk Arpa.
