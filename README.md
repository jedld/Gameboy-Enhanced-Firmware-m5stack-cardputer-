This is a fork of https://github.com/Mr-PauI/Gameboy-Enhanced-Firmware-m5stack-cardputer- with
an emphasis on Gameboy Color support.

Gameboy Emulator; complete with audio, configurable controls, display and performance options, savegames, save states, filesystem navigation and no .gb ROM file size limits imposed by memory. Various other enhancements. Accurate palettes. Partial Super Gameboy Enhancement support, including borders. Extended 12 colour mode, along with all official/original GBC palettes. 44 Analogue Pocket 12 colour community palettes included with automatic mapping of titles to AP palettes partially implemented. DMG titles automatically receive authentic Game Boy Color colourisation, including the original boot-time button combos for palette selection. The Options menu also exposes a stretch-to-width toggle if you prefer filling the Cardputer display over pillarboxed output. A four-slot quick save-state system with on-screen feedback lets you checkpoint and recover progress without leaving the emulator loop.

### Quick save states & status overlay

* Hold `Fn` and tap `1`–`4` to capture a snapshot into the matching slot.
* Hold `Ctrl` and tap `1`–`4` to instantly load that slot.
* Each action confirms with a colour-coded banner (green = saved, grey = empty slot, red = failure) rendered directly over the gameplay framebuffer.
* Slots persist per cartridge and reload automatically after a reset or power cycle; switch to a different ROM to start with fresh slots. Battery-backed saves continue to flush automatically in the background.

## Compiling the firmware for the M5Stack Cardputer

### Prerequisites

* Arduino IDE 2.3 or newer (https://www.arduino.cc/en/software). You can also use the Arduino CLI if you prefer working from the terminal.
* M5Stack board support package `>= 2.1.1`, installed through the Arduino Board Manager.
* Libraries from M5Stack: **M5Unified** (installs **M5GFX** and **M5Cardputer**) via the Arduino Library Manager.
* USB-C cable capable of data transfer for flashing the Cardputer.

### 1. Prepare the sources

Clone the repository together with its submodules so the Peanut-GB core is available locally:

```bash
git clone --recursive https://github.com/Mr-PauI/Gameboy-Enhanced-Firmware-m5stack-cardputer-.git
cd Gameboy-Enhanced-Firmware-m5stack-cardputer-
```

If you already cloned the project without `--recursive`, initialise the submodule before building:

```bash
git submodule update --init --recursive
```

### Build and flash with VS Code + PlatformIO

1. Install Visual Studio Code and add the **PlatformIO IDE** extension (it bundles the PlatformIO CLI).
2. Open this repository folder in VS Code. PlatformIO will detect the `platformio.ini` file automatically.
3. When prompted, allow PlatformIO to install the missing Espressif32 toolchain and dependencies defined in `platformio.ini` (this pulls **M5Unified** for you).
4. Connect the Cardputer via USB-C. If you are on Linux, ensure your user has access to `/dev/ttyACM*` devices (typically by joining the `dialout` group).
5. From the PlatformIO toolbar (bottom status bar) choose the **Build** target to compile. The active environment is `m5stack_cardputer` and is selected automatically.
6. Choose **Upload** to flash the firmware. If the serial port is not detected, click the PlatformIO serial-port selector in the status bar and pick the correct `/dev/ttyACM*` entry.
7. Use **Monitor** (or `PlatformIO: Serial Monitor`) to observe runtime logs at 115200 baud.

You can perform the same actions from the command line without opening VS Code:

```bash
pio run
pio run -t upload --upload-port /dev/ttyACM0
pio device monitor -b 115200
```

Adjust the upload port to match your system. PlatformIO caches toolchains, so these downloads only happen the first time.

#### Optional build flags

- `ENABLE_MBC7` (default `1`) toggles the MBC7 accelerometer/EEPROM emulation. Set it to `0` in `platformio.ini` or provide `-D ENABLE_MBC7=0` on the command line if you want to exclude MBC7 support (for example to shave a little flash or when targeting devices without the tilt sensor).
- `ENABLE_BLUETOOTH` (default `1`) controls whether the firmware initialises the NimBLE stack. Set it to `0` to strip Bluetooth support entirely and reclaim memory.
- `ENABLE_BLUETOOTH_CONTROLLERS` (default `1`) enables the Bluetooth HID controller/keyboard bridge. Set to `0` when you only need other Bluetooth features or want the lightest build.

#### One-command build & upload helper

The repository includes a convenience script that wraps the PlatformIO CLI:

```bash
./scripts/flash_cardputer.sh [-p /dev/ttyACM0]
```

By default it builds and uploads the `m5stack_cardputer` environment. Pass `--no-upload` to compile without flashing, or `-e <env>` if you add more PlatformIO targets later. Set the `PIO_BIN` environment variable when PlatformIO is not available as `pio` on your `$PATH`.

## Bundling ROMs directly into the firmware (optional)

The firmware can embed multiple Game Boy or Game Boy Color ROM images so they boot without streaming data from the SD card. Embedded titles show up in the file picker under a `[FW]` label, and the one flagged for autoboot is marked `[FW★]` and launches automatically when present.

1. Place the ROM file (`*.gb`/`*.gbc`) on your workstation.
2. Add it to the embedded ROM manifest with the helper script:

	```bash
	python3 scripts/embed_rom.py add path/to/YourGame.gbc --name "Custom label" --autoboot
	```

	* The script maintains `embedded_roms.json` (manifest metadata) and stores raw payloads under `embedded_roms/`. Re-running `add` with different options updates the manifest entry in place.
	* Use `--name` to control how the title appears in the firmware menu. Omit `--autoboot` if you want the SD card browser to remain the default boot experience.

3. Rebuild and flash the firmware. The script regenerates `embedded_rom.cpp` and related headers every time you modify the manifest, so no manual edits are required.

### Managing embedded ROMs

The helper script exposes several sub-commands to maintain the manifest:

* `python3 scripts/embed_rom.py list` – show all registered ROMs and their autoboot status.
* `python3 scripts/embed_rom.py remove "Custom label"` – delete an entry by name (the payload file is removed too).
* `python3 scripts/embed_rom.py autoboot "Custom label"` – toggle which entry will launch automatically at startup.
* `python3 scripts/embed_rom.py clear` – wipe the manifest and generated payloads.
* `python3 scripts/embed_rom.py generate` – rebuild the translation units from the existing manifest (handy after resolving merge conflicts).

Remember to respect the legal status of any ROMs you embed—the project does not distribute copyrighted games.

### 2. Install the M5Stack Cardputer board definition (Arduino IDE only)

1. Open the Arduino IDE and go to **File ▸ Preferences**.
2. In **Additional Boards Manager URLs** add `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json` (keep existing entries separated with commas).
3. Open **Tools ▸ Board ▸ Boards Manager…**, search for **M5Stack**, and install the **M5Stack Arduino** core.
4. After installation pick **Tools ▸ Board ▸ M5Stack Arduino ▸ M5Stack-Cardputer**.
5. Ensure that **USB CDC On Boot** is *Enabled* and **PSRAM** is set to *OPI PSRAM* (the defaults for this core).

### 3. Install required libraries (Arduino IDE only)

1. Open **Tools ▸ Manage Libraries…**.
2. Search for **M5Unified** by M5Stack and install the latest version. The dependent libraries (M5GFX, M5Cardputer) are installed automatically.
3. The remaining headers (Peanut-GB, minigb_apu) are bundled with this repository, so no further libraries are required.

### 4. Build and flash from the Arduino IDE

1. Restart the Arduino IDE to ensure the new board and libraries are loaded.
2. Open `gb_cardputer.ino` in the repository root.
3. Connect the Cardputer to your computer via USB-C. On Linux you may need to add your user to the `dialout` group or adjust `udev` permissions for `/dev/ttyACM*` devices.
4. Select the serial port the Cardputer exposes under **Tools ▸ Port**.
5. Click **Verify** to compile. A successful build finishes without errors.
6. Click **Upload** to flash the firmware. The Cardputer resets automatically when the upload completes.

### (Optional) Build with the Arduino CLI

Once the board package is installed you can automate builds without launching the IDE:

```bash
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer gb_cardputer.ino
arduino-cli upload --fqbn m5stack:esp32:m5stack_cardputer -p /dev/ttyACM0 gb_cardputer.ino
```

Adjust the serial port (`-p`) to match your system. The first two commands only need to be executed once per environment.

Gameboy Color support is experimental and there would probably be performance issues, glitches, corruption and unexpected behavior. All `.gb` and `.gbc` files are listed by the file explorer in the current directory. 

Read the instructions at the bottom for controls or refer to the graphic guide below. Forked from original gb_cardputer implementation. No bootloader has been merged with this firmware (at this time), so if you are having issues try using m5launcher to install.

![image](https://github.com/Mr-PauI/Gameboy-Enhanced-Firmware-m5stack-cardputer-/assets/169319235/31c67758-ca57-4217-a3ef-25c20e5cc8f2)


![image](https://github.com/Mr-PauI/Gameboy-Enhanced-Firmware-m5stack-cardputer-/assets/169319235/5c78d59e-ae08-46ed-9522-f11b659504ed)

List of SGB and Analogue Pocket enhanced titles:
* Adventures of Lolo
* Alleyway
* Arcade Classics No 1: Missile Command & Asteroids
* Arcade Classics No 2: Centipede and Millipede
* Balloon kid
* Baseball
* Battlezone/Super Breakout
* Donkey Kong
* Donkey Kong Land
* Dr. Mario
* Dragon Warrior I&II
* F1 race
* Final Fantasy Legend
* Final Fantasy Legend II
* Game & Watch Gallery
* Game Boy Gallery
* Game Boy Wars
* Golf
* James Bond 007
* Kaeru no Tame
* Kirbys Dream Land
* Kirbys pinball land
* Zelda Links Awakening
* Mario and Yoshi
* Mario Land
* Mario Land 2
* Mega Man I
* Mega Man II
* Mega Man III
* Metroid II
* Mystic Quest
* Pocket Bomberman
* Pokemon Red
* Pokemon Blue
* Pokemon Yellow
* Pokemon Gold(limited)
* Pokemon Silver(limited)
* Qix
* Solar Striker
* Space Invaders
* Star Wars: Super Return of the Jedi
* Tennis
* Tetris
* The Rescue of Princess Blobette Starring A Boy and His Blob
* Wario Land
* X
* Yakyuman
* Yoshi

10.10.2025:0.1

* Forked jedld version
* sync with latest peanut_gb
* save states

24.07.2024:0.78

* Reduced overall volume; it still draws enough power to cause the backlight to flicker with the boost on full effect and volume maxed but it was flickering badly with the new amplification code.
* Added revision number to boot sequence
* Added configurable BASS BOOST to settings menu (0 -> +42db gain)
* Removed dynamically allocated memory component for cartridge ram, 32k is largest any compatible game requires anyways so this is now a fixed buffer preventing memory fragmentation which allows the new....
* Added ability to quit the current game and return to the file selection menu; there may be a slight
sound as the next game starts. This is just left over audio buffer info from the previous emulation.
* Spent time this month porting the emulator to T-Display S3 Touch, gamepad support working in that version, this was research to hopefully discover the incompatibility the cardputer seems to be having, power and library conflict are leading contenteders. Code is the same.

07.07.2024:

* Added bass boost function to audio engine
* Added SGB support for pocket bomberman
* Improved audio quality; less clipping at high volume levels.
* Fix bug with startup audio being forced to Mute (part of BLE debugging code I forgot to remove) 
  
06.07.2024:v0.77
* Added adventures of lolo, james bond 007, Megaman 3, The Rescue of Princess Blobette Starring A Boy and His Blob, Game & Watch Gallery, Arcade Classics No2 Centipede and Millipede SGB & AP support
* Increased speed of entering large directories in some circumstances
* Increased file list limit to 1024 entries maximum, provided there is sufficient memory. Most files probably use less than half the max file name size, so 1024x128 directory entry listings are easily accommodated. File names may still be a full 256 bytes in length.

29.06.2024:v0.76
* Compatibiliyy mode settings now take effect immediately if changed mid-game; will display message if mode is locked on due to override requirement (ie: battlezone which requires it on for any functionality)
* Button mapped to controller B and del/back button will return to a parent directory (if not at root)
* Added CRT Television border available for all games
* Added Star Wars: Super Return of the Jedi SGB & Ap support w/Border
* Increased file/directory listing limit to 512 entries per directory
* No longer crashes when there are too many files, will list first 512 files and display warning.
* Added warning message when too many files/directories exist to list.

25.06.2024:v0.75
* Firmware now contains bootloader so it can be loaded without a launcher/direct from m5burner
* Added SGB & AP support for Mega Man I/II and Mystic Quest
* Added USB serial debug output to the entire function chain leading up to the main emulator loop. This should hopefully aid in helping debug games, and provide more comprehensive information for trouble shooting.
* Added CPU mode to settings menu (CPU MODE: FAST/COMPATIBLE), defaults to fast. Enabled automatically for titles known to need this: Battlezone so far, will add any reported titles. For now it takes effect only upon restart of the emulator (will take effect immediately in future update). This disables all speed hacks,and forces rendering of every line of every frame.
* BLE gamepad support only stable in file picker, causes restart during emulation so still disabled.
* Fixed Minor bug with auto SGB mode flag setup
  
24.06.2024:v0.74
* Added compatibility mode that skips all speed hacks (skipped frames and unrenderd lines)
* Added automatic compatibility mode override for titles that are identified to require this, as a result Battlezone is now compatible.
* Added SGB support for Battlezone
* Resolved BLE server and SD card conflict; this took about a week of research and debugging.
  
20.05.2024:v0.73
* BLE startup behaviour implemented, BLE sync from main menu implemented
* Gamepad buttons a,b,x,y and d-pad directional control integrated
  
19.05.2024:v0.72
* Implemented Default Palette selection, SGB manual/auto selection and SGB border options
* Added Default Palette, SGB palette manual/auto, SGB border manual/auto/gboy to options menu
* Added BLE connect manual/auto, BLE button swap on/off (swaps a+b button mapping), BLE deadzone small/med/large for left-stick deadzone configuration
* All new options are now saved in a new iteration of the config file, as usual, any previous config file will automatically migrate to the new version
* BLE connection settings will be saved/loaded regardless of BLE compilation support for consistent configuration file layout
  
18.05.2024:v0.71
* Added SGB profiles for Alleyway, Baseball, Dr. Mario, Game Boy Wars, Kaeru no Tame, Kirbys Dream Land, Mario and Yoshi,Qix,Tennis,X,Yakyuman,Yoshi
(note: not all of these are compatible, but it almost completes the built-in SGB maps that were included in the SGB hardware; future core updates may improve compatibility)
* Added AP profiles for Alleyway, Baseball, Dr. Mario
* Added bluetooth library to the emulator, this has a fairly substantial memory cost and will be disabled until ready for deployment; made inclusion of the BLE code optional when compiling.
* Added initial hooks for gamepad support.
  
12.06.2024:v0.70
* Added per-game savestates (1 state/game in .qsv file), backspace to save, minus/underscore button to load
* All borders are now drawn as RGB565 sprites; almost no impact for FPS display now
* Borders now have a cooldown before changing, allowing the emulator to run smoothly when cycling options
* Increased scroll speed in file selection menu
* Changed number of file/directories displayed at once from 3 to 5
* Added Dragon Warrior I & II SGB & AP support w/ Border
* Added SGB border support for Pokemon Silver/Gold, SGB/AP colours temporarily set to existing pallets until better is found

11.06.2024(pm):v0.68
* Control A and Control Start will now also navigate into directories
* Message Box constrast increased (darker background)
* First item in subdirectories is selected (if anything is present) instead of return to parent directory option
* Added Generic Fantasy Border (from a jp only SGB game) available to all games
* Final Fantasy Legend II SGB * AP support (same as FFL1)
* Added Metroid II SGB & AP support
* Added Golf SGB & AP support
* Added Solar Striker SGB & AP support
* Added Missile Command/Asteroids SGB and AP support w/ border
* Added Space Invaders SGB & SP support w/ Border

10.10.2025:Cardputer port
* Added four quick save-state slots with Fn hotkeys and modifier-based loading.
* Added an in-game status message overlay for save/load feedback and other system notices.
* Save-state buffers now auto-dispose when switching ROMs to reclaim PSRAM instantly.
* Refined LCD scaling so downsampled text retains its full outline on the Cardputer display.

01.10.2025:Cardputer port
* Automatically detects Game Boy Color compatible ROMs on the Cardputer port and enables colour palettes based on the ROM header hash.

11.06.2024(am):v0.67
* Added ability to return to parent directory if it exists
* Added ability to launch ROMs from other directories
* Hides System Volume Information if in root directory; this is probably not what the user wants.
  
10.06.2024:v0.66.5
* Added memsub_malloc() and memsub_freeall() functions to allow paging memory to be used for other purposes:namely, file browsing.
* Changed file_picker() function to use memsub_malloc() / memsub_freeall(), limits are as follows:
256 files/folders per folder, maximum path+file length+3 letter extension is 256 bytes
There is no limit to the depth of the folders, except that the path name+file cannot exceed 256 bytes
No issue with long file names, the memsub_malloc() has 65k roughly allocated per directory listing, just be mindful of the overall path+filename length
* Added *.gbc in addition to *.gb files to selection menu
* Added directories to selection menu, can now explore subdirectories
* Adjusted default SGB palettes for Pokemon Blue/Red; previous fix required adjustment of these two titles.
  
09.06.2024:v0.66
* Added Max and Min audio quality settings, 6 and 72 sample intervals respectively
* Added 12 colour Analogue Pocket palette for Kirbys Pinball Land
* Added 12 colour Analogue Pocket palette for Donkey Kong Land
* Added SGB border and palette for Donkey Kong Land
* Added Cardputer Arcade cabinet border, available for all games
  
08.06.2024:
* Silver has been tested, must rename to .gb from .gbc
  
06.06.2024:v0.65
* increased paged memory limit to 2megs to accommodate Pokemon Gold/Silver
* Gold has been tested, must rename to .gb from .gbc
* increased specificity of error messages regarding paging
  
04.05.2024:v0.64
* Fixed bug with setting initial super gameboy palettes
* Added Pokemon Yellow SGB Border and AP Community Palette
* Added Supermario land 2 SGB border
* Added Tetris SGB border
* Added Final Fantasy Legends SGB Palette and Border, added 12 colour Analogue Pocket palette
* Pokemon Blue border is now scaled using simple pixel scaling method (same as all other borders) to preserve pixelized look
* Gameboy border left border has been adjusted
  
03.05.2024:v0.63
* Added audio quality setting (high,med,low, update intervals of 12,24 and 36 audio samples)
* Added startup audio volume setting (default is mute)
* Added interlacing setting
* Added new settings to configuration files; old configuration files are always supported, and will be upgraded to the newest version when the settings are saved for the first time.
* Squished another bug; timed events weren't taking into account time for border drawing/clearing
* Fixed crash from settings menu when accessed from main menu, settings are saved on game launch or if changed mid game. Crash was related to saving within the file picker.
  
02.05.2024:v0.622
* Added mute and volume half/max contols to Fn+Left/Right keys
  
01.05.2024:v0.621
* Experimented with an inline version of the Game Boy CPU and its various functions but they yielded no performance gain
* CPU step loop moved inside of peanut_gb to reduce function calls, essentially the same as inline but with no memory penalty
  
30.05.2024.v0.62
* Audio rendering has been made faster updating every 24 by default samples, was twelve; 24 will be the default (a.k.a. medium quality) sligt reduction in quality when frame rate dips low can be noticed at 36; may make option for audio quality. Updating every 36 samples seems
to be the minimum(clicking becomes more likely beyond this update interval), so 12,24,36 seem to be appropriate settings; every 12 sameples was used in 6.0 may added higher and lower quality settings in future
* Adjusted TTL logic to run seperately from the page retrieval so that retrieving data is as fast as possible when in memory.
  
29.05.2024:v0.61
* Memory solution devised was based on two ideas: Assume stastically random access patterns, and place no limits on addressable space.
The reality is that the gameboy games never exceed 8megabits (1megabyte); Taking advantage of the limitations of the gameboy, I have removed the generalized nature of the	previous solution in favor of one that provides a deterministic approach to page seeking, with the tradeoff that no games larger than a real game boy game can be loaded any longer. This should not pose an issue for any (valid) title. This has increased frame rates slightly all around and made them much steadier; page location has no bearing on access times. Page seek times are fixed, and a minor amount of page maintenance is done at these intervals, distributed across successive calls still. This would be deterministic page seeking mixed with the previous progressive pruning.
* Added Error messages with regards to writing save game files, attempts to do so when there is no cart ram
* Added Error messages specific to the case of being unable to open a file vs. file size 0 (was ambiguous before)
* Removed pause associated with automatic savegames; bars operate on a timer now and will disappear 1second after pending write is completed

28.05.2024:v0.601
* No longer will create save files or allow save files to be created for titles without cart ram(0b files no more)


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
