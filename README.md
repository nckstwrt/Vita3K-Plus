# Vita3K+

A fork of Vita3K with additional game compatibility and rendering fixes. All credit for the emulator itself belongs to the Vita3K team.

## Download

https://github.com/nckstwrt/Vita3K-Plus/releases

## Android notes

> **Snapdragon / Adreno:** Use Turnip drivers for the best compatibility.

> **Freezing or hangs:** Enable **Accurate Thread Scheduling** in Graphics settings.  
> This is needed for games such as *Dead or Alive 5 Plus*, *Sonic*, *Samurai Warriors 3*, etc.

## Enhancements

# v1.1 (Not Yet Released)
- **Dead Or Alive 5 Plus** - Working - On Android requires Adreno, Turnip and the new setting Thread Scheduling Accuracy On
- **Metal Gear Solid 3** - Freezes fixed/worked around. Blending graphics fixed. On-Screen Touchpad fixes.
- **Resistance** - Rendering issues fixed on Android
- **Assassin's Creed III: Liberation**: Sound issues resolved
- **Killzone** - Fix for the crash occurring at level 5
- **Neptunia Re;Birth1** - Fix for crash after selecting New Game and background images not showing
- **Little Big Planet** - "Vignette" shading and background images fixed
- **Ys: Memories of Celcetta** - Graphics Fixed
- **Ragnarok Odyssey ACE** - Graphics Fixed
- **Ninja Gaiden Sigma 2 Plus** - Graphics Fixed
- **ModNation: Racers Road Trip** - Regression Fixed
- **RESOGUN** - Graphics issues now fixed
- **Omega Labyrinth** - Loading fixed by nishinji
- **Dynasty Warriors 8** - Fixed text/graphics clipping
- **Samurai Warriors 3** - Use Accurate Thread Scheduling to workaround loading bugs
- **Gundam Breaker 3** - Almost all fixed. Still issues with main character's graphics.
- **SteamWorld Dig** - Loads and plays
- **UPPERS** - Graphics Fixed
- **Madden 13** - Face textures fixed
- **Oddworld: New'n'Tasty** - Graphics Fixed
- Defaults now to External Host for Windows and Native Buffer on Android. **Use these Memory Mapping options for the best compatibility**
- Lots of memory, timing and sound improvements
- Thanks to Reyes for all his help testing!

# v1.0
- **Killzone** Plays well and looks amazing
- **Resistance** Colours are now all good. Intermittent freezing has been fixed.
- **Assassin's Creed III: Liberation**: Graphical issues fixed
- **LittleBigPlanet**: Missing backgrounds, starting sounds and other graphics fixed
- **Need for Speed: Most Wanted**: floating badges fixed, headlights through the road fixed, sound fixed (mostly).
- **Call of Duty: Declassified** Boots and plays well now
- **Uncharted** Now plays at any resolution plus minor fixes
- **Devious Dungeon** Now launches fine
- **Sonic Transformed** Plays well and looks amazing (new per game thread accuracy setting to ensure a good startup)
- **Spider-Man** No longer sometimes hangs on load.
- **Helldivers** No longer sometimes crashes at startup
- **Soul Sacrifice Delta**: Graphical issues fixed
- **Minecraft** was a white screen in Vulkan, now fixed
- **Kancolle Kai** Now plays without getting stuck at the difficulty selection screen
  
Hopefully a lot of crash bugs and freezing have been fixed by these changes. Other games may now be a lot better or possibly worse with this version.

## Notes

- The settings now default to High Quality
- These updates were designed with higher-end devices in mind. Although I have attempted to make them work and tested on Mali based devices (i.e. Killzone works fine on a Mali-G78 MP14 device I have) that is not the goal of this fork.
- For better character shadows in Killzone use a memory mapping of External Host on Windows or Native Buffer on Android. Double Buffer works well but makes the shadows flicker annonyingly.

## New Screenshots

See the **[Screenshots page](./Screenshots.md)** for the games above running in Vita3K+.

[![Vita3K+ screenshots](./screenshots/KZ.jpg)](./Screenshots.md)

## Report Issues and Problems

https://github.com/nckstwrt/Vita3K-Plus/issues

## Code Changes
Check the all-enhancements branch for the all code changes

Below is the original Vita3k's README:
---

# Vita3K

[![C/C++ CI](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Vita3K/Vita3K/actions/workflows/c-cpp.yml)
[![Release](https://img.shields.io/github/v/release/Vita3K/Vita3K-builds?include_prereleases)](https://github.com/Vita3K/Vita3K/releases)
[![Vita3K discord server](https://img.shields.io/discord/408916678911459329?color=5865F2&label=Vita3K%20&logo=discord&logoColor=white)](https://discord.gg/6aGwQzh)

## Introduction

Vita3K is an experimental PlayStation Vita emulator for Windows, Linux, macOS and Android.

* [Website](https://vita3k.org/) (information for users)
* [Wiki](https://github.com/Vita3K/Vita3K/wiki) (information for developers)
* [Discord server](https://discord.gg/MaWhJVH) (recommended)

## Compatibility

The emulator currently runs most homebrew programs and commercial games.

- [Homebrew compatibility page](https://vita3k.org/compatibility-homebrew.html)
- [Commercial compatibility page](https://vita3k.org/compatibility.html)

## Gallery

|               **Persona 4 Golden** by Atlus                   |                     **A Rose in the Twilight** by Nippon Ichi Software                         |
| :-----------------------------------------------------------: | :--------------------------------------------------------------------------------------------: |
| ![Persona 4 Golden screenshot](./_readme/screenshots/P4G.png) | ![A Rose in the Twilight screenshot](./_readme/screenshots/A%20Rose%20in%20the%20Twilight.png) |

|                  **Alone with You** by Benjamin Rivers                     |                 **VA-11 HALL-A** by Sukeban Games                    |
| :------------------------------------------------------------------------: | :------------------------------------------------------------------: |
| ![Alone with You screenshot](./_readme/screenshots/Alone%20With%20You.png) | ![VA-11 HALL-A screenshot](./_readme/screenshots/VA-11%20HALL-A.png) |

|              **Fruit Ninja** by Halfbrick Studios                  |                **Jetpack Joyride** by Halfbrick Studios                    |
| :----------------------------------------------------------------: | :------------------------------------------------------------------------: |
| ![Fruit Ninja Screenshot](./_readme/screenshots/Fruit%20Ninja.png) | ![Jetpack Joyride Screenshot](./_readme/screenshots/Jetpack%20Joyride.png) |

## License

Vita3K is licensed under the **GPLv2** license. This is largely dictated by external dependencies, most notably Unicorn.

## Downloads

You can download the latest builds from [here](https://github.com/Vita3K/Vita3K/releases/tag/continuous).

* Windows
  * Requirements:
    * [Microsoft Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
* Linux
  * Arch based:
    * [vita3k-bin](https://aur.archlinux.org/packages/vita3k-bin)<sup><small>AUR</small></sup>
    * [vita3k-git](https://aur.archlinux.org/packages/vita3k-git)<sup><small>AUR</small></sup>
  * Requirements:
    * xdg-desktop-portal
* Android
    * [Adreno drivers](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/)
* Others
  * [Download Artifact](https://github.com/Vita3K/Vita3K/actions?query=event%3Apush+is%3Asuccess+branch%3Amaster)
  * [Old builds](https://github.com/Vita3K/Vita3K-builds/releases)

## Building

Please see [`building.md`](./building.md).

## Running
Check our [quickstart guide](https://vita3k.org/quickstart) to make sure your computer meets the minimum requirements to run Vita3K.  
Don't forget to have your graphics driver up to date and to install the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/VC_redist.x64.exe) if you are a Windows user.  

## Bugs and issues
The project is in an early stage, so please be mindful when opening new issues. Expect crashes, glitches, low compatibility and poor performance.

## Thanks
Thanks go out to people who offered advice or otherwise made this project possible, such as Davee, korruptor, Rinnegatamante, ScHlAuChi, Simon Kilroy, TheFlow, xerpi, xyz, Yifan Lu and many others.

## Donations
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/vita3k)
<br>
Thank you to the supporters and to all those who support us on our [ko-fi](https://ko-fi.com/vita3K).
* Among them, those who subscribed to the Nibble Tier and upper: **j0hnnybrav0, Mored4u, TacoOblivion, Undeadbob and uplush**

## Note
The purpose of this emulator is not to enable illegal activity. You can dump games from a Vita by using [NoNpDrm](https://github.com/TheOfficialFloW/NoNpDrm) or [FAGDec](https://github.com/CelesteBlue-dev/PSVita-RE-tools/tree/master/FAGDec/build). You can get homebrew programs from [VitaDB](https://www.rinnegatamante.eu/vitadb/#/).

PlayStation, PlayStation Vita and PlayStation Network are all registered trademarks of Sony Interactive Entertainment Inc. This emulator is not related to or endorsed by Sony, or derived from confidential materials belonging to Sony.
