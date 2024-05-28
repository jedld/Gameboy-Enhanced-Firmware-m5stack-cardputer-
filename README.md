*Note: There has been some confusion regarding my releases on Github and source code. I have not uploaded any source code as plans have changed along the way. Originally I was going to polish the implementation and release it to be finished by the community, but just as I begun the project was archived by the original author. This resulted in a very low-effort release of the same firmware without credit to the original author for their implementation landing on m5burner at approximately the same time as my version. As this is my return to coding after a 14 year hiatus, I've decided that rather than have my additions copied with no meaningful contributions or acknowledgement, that I'd just finish the entire emulator myself and release source code after I have finished my work and had my fun with this project. This seems preferable to having my passion for the project deflated. I've been sharing detailed change logs, and under /rCardputer on reddit I am always willing to go into further detail of the specifics of the solutions I have employed in the interest of helping the budding developer.

![image](https://github.com/Mr-PauI/gb_cardputer_mod/assets/169319235/5a9bf85b-0a44-4f37-931b-c06ac70d62a3)

Updates are more frequent on m5burner than github, but I'm trying to drop a binary here now and then.

Gameboy Emulator; Now with sound. Has configurable controls, no limit on ROM file size and provides savegame support. Various other enhancements. Accurate palettes. Partial Super Gameboy Enhancement support, including some borders. Extended 12 colour mode, along with all official/original GBC palettes. 44 community Analogue Pocket 12-color palettes with automatic mapping of games to AP profiles partially implemented.

ROM files must be placed in the root directory of your compatible SD card (fat32/sdhc). Read the instructions at the bottom for controls. Forked from original gb_cardputer implementation, which deserves everlasting credit for providing me simultaneously with a conceptual understanding of peanut_gb and a renewed passion for some well crafted logic.

27.05.2024:v0.6
* Audio is fully implemented. Inaccuratley, built for speed. Hardly any impact of rendering.
Note: While the tones will not change pitch as a title slows down, music will speed up and slow down along with frame rates.
* Implemented volume up/down controls attached to Fn+up/down arrows. If these are mapped to controls, at this time, it will respond to both.
Further Note: I put no limits on control configuration. Make everything a single button if you want.
* Added cessation of audio rendering when volume is set to 0.

26.05.2024:v0.57
* alterered APU to allow variable amounts of time between calls
* Tuned APU performance to get first recognizable gameplay sounds out of the emulator.

25.05.2024:v0.56
* Added 12color AP palette mapping for Links Awakening
* Added SGB border for Pokemon Red
* Added SGB border for Links Awakening
* Added notification for titles with Analogue Pocket profile, but no SGB profile
* Work on audio playback engine has begun, core 0 continusoly feeds audio from a buffer to the speaker; a test 1000hz sine wave shows little to no stuttering while emulator runs, needs tuning. Disabled until apu work complete.

23.05.2024:v0.53
* Required memory for pokemon +5k just incase has been made available
* Implemented 12-color Analogue Pocket palettes for Pokemon Red/Blue
* Implemented preset SGB and GBC palettes for Pokemon Red/Blue
* Implemented SGB borders for Pokemon Blue
* disabled audio for this release; exported to wav file to examine and while its the same code
it is not generating the same output as the PC test version. Will take further research.

22.05.2025:v0.52
* Implemented audio with peanut_gb hooks on PC (version for debugging); verified APU core and waveforms
* Integrated Audio with peanut_gb hooks on ESP32; audio waveforms are now being generated, needs balancing with CPU and speaker playback still

21.05.2024:v0.51
* implemented main emulator function as a task on core 1 and a second persistent task on core 0 via RTOS; this is in preparation to acommodate the APU engine
core 0 thread must call vTaskDelay as there are vital threads on that core, which if not attented to, will cause a restart. I dont have full use of this core.
discovered pokemon starts working if I have more free memory so I suspect I run out of stack space. 180k paging it works 200 it doesn't will calculate requied amount

20.05.2024:v0.5
* added bottom menu bar with instructions to the main rom selection menu, pres ESC or ' key (same thing) to enter the settings menu either from the main menu or while playing a game
* added options menu, can be entered from the menu or from within a game; displays the 8 main controls with their current setting, saves settings when closed
* added restore defaults for config menu
settings are saved to gbconfig.dat; delete this if you are having any issues

19.05.2024:v0.492
* Reimplemented memory subsystem to use progressive partial page seeking/pruning; the original memory management code was the first I had typed in 14 years; after some thought I devised something much more suitable for a real time environment. This resulted in the average page seek time being much lower, and distributes maintenance of the paging system across successive calls. The results are the largest improvement to speed to date. Over double the frame rates from before; less stutter, smoother page transitions in memory. Donkey Kong Land averages around 45fps now; Super Mario land 2 gets an average 53fps on the overworld.

18.05.2024:v0.491
* Reduced rendering workload 6.25% by modifying peanut_gb to inherently skip lines that aren't visible due to scaling.
This prevents the engine from having to process the layer and sprite data for these lines all together. Why 6.25%? because 9 lines are skipped which is 6.25% of the lines that were rendered previously.

17.05.2024:v0.49
* Refactored graphics code; ~120,000 less operations a frame
* Scrolling behaviour and screen content differences no longer effect rendering performance Titles such as final fantasy or even super mario land 2 show a huge improvement in overworld movement speed.
17.05.2024: v0.483 Pushing this now as a BUG FIX RELEASE
* Fixed bug in main menu causing selection to only move upward; you can now navigate properly again.
* added FPS display while Fn button is held, causes slow down which is subtracted from the FPS display. This is so I can evaluate performance improvements more than anything else, but it doesn't hurt to have so I'm leaving it in.
* new borders are on hold while I make decisions about the internal format (leaning towards argb1555,presently 565)
16.05.2024: v0.482
* Added Analogue Pocket 12color palette category with 44 palettes
* Automatic 12color(AP) palettes mapped as per Analogue Pocket suggested mappings for:
Mario 1/2/Wario Land/Balloon Kid/F1 Race/Tetris
* added Cottage Daytime SGB border
* Fixed 12colours not being assigned until palette select bug
* regularly mapped controller up/down/a now functions in addition to arrow/enter keys in main menu to allow a single hand posture for the entire interface if desired.
15.05.2024: v0.481
* proper startup sequence, may help compatibility with some games.
* Message boxes now will display any emulation errors reported by peanut_gb
* Attempts to access ROM address outside of the available cartridge ROM will display an appropriate message
14.05.2024 v0.48
* Added 12 colour mode, with the 12 palettes the game boy colour could apply to old game boy games as the first of this mode, but more to come in this area.
* Super gameboy support added for screen borders with gameboy skin set as default, followed by 1 (for now) of the official borders from the hardware itself. Activate by holding Fn and pressing '[' and cycle borders by holding Fn and pressing ']'. Because these are the same keys used for other visual modifications, I hope using the combination of Fn and these keys is intuitive.
* Super gameboy palette support added for balloon kid, the legend of zelda links awakening and kirbys pinball land
* Various other small tweaks you probably wont notice.
12.06.2025 V0.47
* Added first iteration of Super Gameboy Mode with all 32 official palettes that were included with the original hardware. Nintendo included a table on the device to map certain games to certain palettes, and that functionality is partially implemented. Mario 1/2/Wario Land, F1 Racer and Tetris all autodetect and assign their colour scheme. This mode can be toggled on and off at any time during play with the '[' button, and the current cycle palette button will cycle through the 32 included palettes. Games with defined profiles with start with that palette selected automatically when Super Gameboy Mode is engaged for the first time each session.
*Lots of user interface changes; message boxes will appear to describe your palette selections, among other things - no console style debug remains in normal operation.
*Another 4k of memory allocated to ROM storage, may smooth out some edge cases of stuttering.
*Improved readability in the main menu; and made highlighted selection more apparent.
*Added smooth transitions to splash screen.
*Disabled unused (for the moment) configuration file

11,05.2024:: 0.44 
* squished a bug, added palette control, press ']' to cycle between presets.
 b/w, gameboy(original), gameboy pocket and gameboy light (in that order).
* Huge performance improvements for larger ROMs with over 110k more ram available to the memory sub-system. Palette values are from https://en.wikipedia.org/wiki/List_of_video_game_console_palettes for accuracy.

10.05.2024:: 0.41 
*added savegame support. if a game uses it's onboard ram constantly though, use the manual backup button (=). The save feature will only automatically engage after the cartridge ram has been left untouched for a second.

Yellow bars on either side of the display momentarily indicate that the cartridge ram has been backed up. The savegame format will not be changing, its a simple binary dump.

Controls(in game):
left/right use 'a' and 'd'
up/down use 'e' and 's'
A/B use 'l' and 'k'
start/select use '1' and '0'
cycle palette in current mode use the ] square bracket
cycle between classic gameboy,super gameboy & 12 color modes [
force cart ram backup press '=' (if uncertain of gamesave use this)
turn on super gameboy border at any time by pressing Fn+'['
cycle through border options by pressing Fn+']'
Display current FPS by holding Fn; it will appear in 1 second and update every second there after, can cause some slowdown but that is accounted for in the FPS count.
Press `/esc in the main menu or during gameplay for settings menu

Have fun!

## Original Project Readme ##

# GB Cardputer
Run GameBoy games on your M5Stack Cardputer!*

Uses [Peanut-GB](https://github.com/deltabeard/Peanut-GB), a super cool project that's basically an entire GameBoy emulator in a single C header!

# Warning
This "port" is not polished at all, the code needs major commenting, refactoring and other general cleanup.

Audio also doesn't work, since I just didn't feel like making it work for now lol, though I might addreess that later on

I just cobbled this together between yesterday (17/1/2024) and today just to mess around with the Cardputer a bit.
