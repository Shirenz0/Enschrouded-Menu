<div align="center">

# E-Menu

**An in-game mod menu for Enshrouded, drawn right inside the game.**

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0b0d14?style=for-the-badge&logo=windows&logoColor=white)
![Renderer](https://img.shields.io/badge/Vulkan-implicit%20layer-f59e38?style=for-the-badge&logo=vulkan&logoColor=white)
![Language](https://img.shields.io/badge/C%2B%2B-20-61d4c6?style=for-the-badge&logo=cplusplus&logoColor=white)
![Game](https://img.shields.io/badge/Enshrouded-1.0-f59e38?style=for-the-badge)

<br><br>

<img src="docs/images/menu-tabs.gif" alt="E-Menu switching through its tabs" width="408">

<br><br>

<table>
  <tr>
    <td align="center"><img src="docs/images/panel-player.png" alt="Player tab" width="260"><br><b>Player</b></td>
    <td align="center"><img src="docs/images/panel-move.png" alt="Move tab" width="260"><br><b>Move</b></td>
    <td align="center"><img src="docs/images/panel-world.png" alt="World tab" width="260"><br><b>World</b></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/panel-build.png" alt="Build tab" width="260"><br><b>Build</b></td>
    <td align="center"><img src="docs/images/panel-camera.png" alt="Camera tab" width="260"><br><b>Camera</b></td>
    <td align="center"><img src="docs/images/panel-mods.png" alt="Mods tab" width="260"><br><b>Mods</b></td>
  </tr>
</table>

</div>

---

## What it is

E-Menu is a Vulkan implicit layer that adds a glass style overlay menu to Enshrouded. Press **F2** (or **L3 + R3** on a controller) and the menu slides in on the right side of the screen. Every option takes effect immediately, and switching it off restores the original game code byte for byte.

- **No game files are modified.** The menu lives in its own DLL and is loaded by the Vulkan loader.
- **Pass-through everywhere else.** The layer only activates inside `enshrouded.exe`; every other Vulkan app sees an untouched pass-through layer.
- **Safe patching.** Every hook first verifies the original bytes. If a game update changed them, the option shows `N/A` instead of patching blind.
- **HDR aware.** On HDR10 displays the menu colours are re-encoded to PQ so white stays white instead of blinding.
- **Full controller support**, including Steam Input: the game stops receiving pad input while the menu is open.

## Features

| Tab | Options |
|---|---|
| **Player** | God Mode, One-Hit Kill, Infinite Stamina, Infinite Mana, No Fall Damage, Safe Fall Height, Infinite Oxygen, Infinite Frost, Infinite Shroud Timer |
| **Move** | Run Speed, High Jump, Gravity, Fall Speed, Infinite Jumps, Fly (Build Mode), Fly (Look Direction), Walk Under Water, Swim in Air, Climb Any Slope, Glider Freedom, Teleport (save, return, up, down) |
| **World** | Always Day, Transparent Water, Invisible Water, Water Glow |
| **Build** | Build Outside Altar, Build in Shroud, Build in Crypts, Altar From Any Distance, Build Range, Block Material, Free Crafting, No Durability Loss, Max Stack Size |
| **Camera** | First Person, Stable Camera Distance, Camera Distance, Center Camera |
| **Mods** | Toggle for a Skill Tree data mod (applies after a restart) |

Sliders return to their default value to switch an option off. Toggles with a value (Water Glow, Camera Distance, Block Material, Max Stack Size, Safe Fall Height, Fly) show their slider once switched on.

## Cheat reference

Open the menu with `F2` (or `L3 + R3`), pick a tab, and click a row (or press `A`) to switch it on or off. There are three kinds of rows:

- **Toggle:** on or off.
- **Slider:** drag it (or use left / right on a controller). The option is off while the slider sits at its default value and on as soon as you move it.
- **Toggle with a value:** switch it on first; a slider appears below it.

An orange bar on the left of a row means the option is active. The header shows how many options are on. Switching an option off puts the game's original code back.

### Player

| Option | What it does | How to use |
|---|---|---|
| **God Mode** | Your character takes no damage. Healing still works. | Toggle. |
| **One-Hit Kill** | Any damage you deal to an enemy or creature drops its health to zero. | Toggle. |
| **Infinite Stamina** | Stamina is kept at its maximum, so sprinting, climbing and dodging never run out. | Toggle. |
| **Infinite Mana** | Mana is kept at its maximum, so spells can be cast without limit. | Toggle. |
| **No Fall Damage** | Landing from any height costs no health. | Toggle. |
| **Safe Fall Height** | Sets the height from which a fall starts to hurt. The game default is 8 m, with full damage from 16 m; the slider sets the first value and the second becomes double. | Toggle, then set 8 to 200 m (default 50 m). Switching it off restores 8 / 16 m. |
| **Infinite Oxygen** | Your breath under water never runs out. | Toggle. |
| **Infinite Frost** | The cold timer never runs out, so you do not freeze in cold areas. | Toggle. |
| **Infinite Shroud Timer** | The Shroud timer is held at 300 and never counts down, so you can stay in the Shroud indefinitely. | Toggle. |

### Move

| Option | What it does | How to use |
|---|---|---|
| **Run Speed** | Multiplies your horizontal movement speed. Jumping and falling are not affected. | Slider 1.0x to 3.0x (1.0x = off). |
| **High Jump** | Multiplies the upward speed of a jump. | Slider 1.0x to 5.0x (1.0x = off). |
| **Gravity** | Sets the gravity value. Lower is floatier; 0 means no gravity at all. | Slider 0 to 25 (25 = game default, off). |
| **Fall Speed** | Scales the maximum falling speed, so you sink slowly instead of dropping. | Slider 0.05x to 1.00x (1.00x = off). |
| **Infinite Jumps (E)** | The game no longer marks your jump as used, so you can jump again while in the air and keep climbing. | Toggle, then keep pressing jump in mid-air. |
| **Fly (Build Mode)** | Replaces your vertical speed with a small constant upward speed, so you float and slowly rise instead of falling. | Toggle. Switch it off to come back down. |
| **Fly (Look Direction)** | Your vertical movement follows the camera: look up to rise, look down to descend, while moving as normal. | Toggle, then set the speed 1.0 to 20.0 (default 7.5). |
| **Walk Under Water** | The game no longer switches you to swimming, so you walk along the bottom of lakes and the sea. | Toggle. Combine with Infinite Oxygen. |
| **Swim in Air** | Puts you in the swimming state everywhere, so you can swim through the air in any direction. | Toggle. |
| **Climb Any Slope** | Raises the game's slope limits (45 / 55 / 65 degrees) to almost 90 degrees, so you can walk up near-vertical terrain. | Toggle. Switching it off restores the original limits. |
| **Glider Freedom** | Lifts the glider's pitch limits so you can point it upward and climb, and changes how fast it turns. | Toggle, then open the glider. |
| **Teleport** | Shows your current position (X, Y, Z). **Save** stores it, **Teleport** takes you back to the saved spot, **Up 5** / **Down 5** move you 5 units up or down. | Buttons. The position appears once you have moved a little after loading in. |

### World

| Option | What it does | How to use |
|---|---|---|
| **Always Day** | Holds the world clock at 12:00 midday. | Toggle. The clock continues when switched off. |
| **Freeze Time** | Not available yet; shown as `SOON`. | |
| **Transparent Water** | Makes water see-through. | Toggle. |
| **Invisible Water** | Water is not drawn at all; you still swim in it. | Toggle. |
| **Water Glow** | Sets how strongly water glows. | Toggle, then set 0 to 50 (default 25). |

### Build

| Option | What it does | How to use |
|---|---|---|
| **Build Outside Altar** | You can build outside the area of a Flame Altar. | Toggle. |
| **Build in Shroud** | You can build inside the Shroud. | Toggle. |
| **Build in Crypts** | You can build inside crypts and other closed areas. | Toggle. |
| **Altar From Any Distance** | The game treats you as standing in range of a Flame Altar wherever you are. | Toggle. |
| **Build Range** | How far away from you build parts can be placed. | Slider 3 to 32 (3 = game default, off). |
| **Block Material** | Every block you place uses the chosen material, whatever is selected in the build menu. The value is the game's internal material id. | Toggle, then pick an id from 0 to 255 (default 167). |
| **Free Crafting** | Recipes cost no materials. | Toggle. |
| **No Durability Loss** | Weapons and tools no longer wear down. Repairing still works. | Toggle. |
| **Max Stack Size** | Raises how many items fit in one stack. Only items that already stack are affected; single items stay single. | Toggle, then set 10 to 9990 (default 999). |

### Camera

| Option | What it does | How to use |
|---|---|---|
| **First Person** | Switches the camera to first person. | Toggle. |
| **Stable Camera Distance** | Stops the game from rescaling the camera distance on its own. | Toggle. |
| **Camera Distance** | Sets how far the camera sits behind your character. | Toggle, then set 0 to 20 (default 4). |
| **Center Camera** | Removes the over-the-shoulder offset, so the camera sits straight behind your character. | Toggle. |

### Mods: Skill Tree Mod

The **Skill Tree Mod (Beta)** row switches the *Free Skills & Upgrades* data mod on or off. With it on, **the entire skill tree is unlocked**: every skill node costs zero skill points, so you can pick every node in the tree instead of choosing a build. Depending on its own settings, the mod can also give extra skill points per level, make upgrade recipes and gear upgrades free and speed up XP gain.

- The mod is separate from E-Menu: it runs through the Enshrouded mod loader and lives in the game folder under `mods\free-skills\`. Without it the row shows `N/A`.
- The toggle only changes the `enabled=` line in `mods\free-skills\config.txt`. Everything else in that file stays as it is; edit it there to change the mod's own options.
- Mods are loaded when the game starts, so **restart the game** after switching it. The row shows "Restart game to apply" as a reminder.

## Screenshots

<img src="docs/images/menu-toggles.gif" alt="Switching options on while playing" width="450">

| | |
|---|---|
| ![Player tab in game](docs/images/menu-player.png) | ![Move tab in game](docs/images/menu-move.png) |
| ![World tab in game](docs/images/menu-world.png) | ![Build tab in game](docs/images/menu-build.png) |
| ![Camera tab in game](docs/images/menu-camera.png) | ![Mods tab in game](docs/images/menu-mods.png) |

![Flying over the landscape](docs/images/flight.gif)

![In game, menu closed](docs/images/in-game.png)

## Controls

| | Keyboard and mouse | Controller |
|---|---|---|
| Open / close | `F2` | `L3 + R3` (both sticks pressed), `B` closes |
| Move between rows | mouse | D-pad or left stick |
| Toggle / press | click | `A` |
| Change a slider | drag | left / right (hold to speed up) |
| Switch tab | click the tab | `LB` / `RB` |

While the menu is open the game does not receive keyboard, mouse or controller input, so your character stays put.

## Installation

### Installer (recommended)

1. Download `EnshroudedMenu-<version>.msi` from [Releases](../../releases/latest).
2. Run it. It installs for your Windows user only and needs no administrator rights. Nothing is placed in the game folder.
3. Start Enshrouded. A small `E-Menu` toast appears in the top right corner; press `F2`.

The installer is not code-signed, so Windows SmartScreen may show "Windows protected your PC". Click **More info**, then **Run anyway**.

**Uninstall:** Windows Settings → Apps → Installed apps → *E-Menu for Enshrouded* → Uninstall.
**Update:** run the newer `.msi`; it replaces the old version.

### Manual install

E-Menu is a regular Vulkan implicit layer: one DLL plus its JSON manifest. The `.zip` on the Releases page contains both plus an `INSTALL.txt`.

1. Build `EnshroudedMenu.dll` (see below), or take it from the release `.zip`.
2. Put `EnshroudedMenu.dll` and `layer/EnshroudedMenu.json` together in a folder, for example `%LOCALAPPDATA%\EnshroudedMenu\layer\`.
3. Register the manifest for your user: add a `DWORD` value named after the full path of the JSON file, with data `0`, under

   ```
   HKEY_CURRENT_USER\Software\Khronos\Vulkan\ImplicitLayers
   ```

4. Start Enshrouded. A small `E-Menu` toast appears in the top right corner; press `F2`.

**Uninstall:** delete the registry value and the folder.
**Disable temporarily:** set the environment variable `DISABLE_ENSHROUDED_MENU=1`.
**Log file:** `%LOCALAPPDATA%\EnshroudedMenu\menu.log`

## Building

Requirements:

- Windows x64, MSVC (Visual Studio 2022 or newer), C++20
- [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9 with `backends/imgui_impl_vulkan`
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.304

Build a 64-bit DLL from `src/*.cpp` plus the ImGui core and Vulkan backend sources, with:

- defines `VK_NO_PROTOTYPES`, `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, `VK_USE_PLATFORM_WIN32_KHR`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`
- `src/exports.def` as module definition file (it exports the layer entry points)
- link `user32`, `shell32`, `dinput8`, `dxguid`
- static CRT (`/MT`) recommended, so the DLL has no runtime dependency

Name the output `EnshroudedMenu.dll` so it matches `library_path` in the manifest.

## How it works

```
 enshrouded.exe ──► Vulkan loader ──► E-Menu layer ──► driver
                                       │
                                       ├─ vkQueuePresentKHR: draws the ImGui menu on the swapchain image
                                       ├─ input: raw input / cursor / Steam Input are paused while open
                                       └─ patch engine: pattern scan, code caves, verified writes
```

| Part | File |
|---|---|
| Layer entry points, instance and device tracking | `src/layer.cpp` |
| Overlay rendering, swapchain resources, HDR10 encoding | `src/overlay.cpp` |
| Menu look, tabs, rows, controller focus | `src/menu.cpp` |
| Option registry and all game hooks | `src/cheats.cpp` |
| Pattern scan, near allocation, thread-safe code writes, x64 emitter | `src/patch.cpp` |
| Keyboard, mouse and raw input handling | `src/input.cpp` |
| Controller reading and XInput hook | `src/gamepad.cpp` |
| Steam Input event interception | `src/steaminput.cpp` |
| Skill Tree mod toggle | `src/mods.cpp` |

Code is only patched while every other thread of the game is suspended and none of them is executing the bytes being replaced. Hooked functions jump into code caves placed within ±2 GB of the game module; all constants and slider values live in that cave page, never inside game structures. The one exception is Safe Fall Height: the fall heights sit in read-only game memory, so the cave only records where they are and the menu writes them once per frame (briefly unprotecting the range), restoring the originals when switched off.

## Notes

- Built and tested against the Steam version of Enshrouded 1.0.
- Meant for single player and your own worlds. On servers you do not own, the host decides what counts.
- A game update can move code around. Options whose patterns no longer match show `N/A` and stay off.
- Not affiliated with or endorsed by Keen Games.
