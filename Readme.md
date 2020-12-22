# DIY Musicbox

This is a complete rewrite of Thorsten Voß [TonUINO](https://github.com/xfjx/TonUINO) project.

While building the project for myself and trying to integrate various
additional features, I figured that the code required some refactoring. As
usual with refactoring, you start at a small part and end up refactoring the
whole thing ;)

Hence, I changed the original code from the usual C-style way of programming to
a more C++-style. There is now an event loop with events for the different
inputs (button presses, track finished, new RFID card, …) which than each call
a handler that again calls a specific function on the currently active system
mode.

All in all, I ended up removing most of the free standing functions and magic
numbers spread throughout the whole code and replaced them with proper
abstraction and/or polymorphism. I hope this makes the code more stream lined
and easier to extend in the future.

While I tried to replicate all the existing features form the original project,
I am not yet fully done. Some of the latest features (such as toddler mode,
freeze dance, …) are not supported. These are things that I will work on in the
near future. However, the basic features as for example reading the RFID card
and playing the corresponding files is fully functional. See the below list for
a complete overview about the finished features. I also added a couple of new
features or changed how things worked.

## Original features
 * [x] Button interaction
   * [x] 3 Button setup
   * [x] 5 Button setup
 * [ ] Play folder
   * [x] Album mode
   * [x] Playbook mode (will save progress in EEPROM)
   * [x] Repeat one mode
   * [x] Party mode
   * [ ] Album mode (von - bis)
   * [ ] Playbook mode (von - bis)
 * [x] RFID cards
   * [x] Read RFID cards (same format as original code)
   * [x] Write RFID cards (same format as original code)
 * [x] Admin mode
   * [x] Program new cards
   * [x] min/max volume
   * [ ] ~~Pin code~~
   * [x] Admin card
   * [ ] ~~Math question to unlock~~
   * [ ] ~~Enter with key stroke~~
   * [x] Say menu options
 * [x] Settings
   * [x] Min/Max volume
   * [x] Last/initial volume
   * [x] Equalizer
   * [x] TonUINO is locked
   * [x] Last active card/folder (this allows resuming the last playback after standby)
   * [x] Audiobook progress
 * [ ] Additional system modes
   * [ ] Freeze Dance
   * [ ] Toddler
   * [ ] Kindergarten

## Additional features
 * Lock and Unlock cards
 * Status LED
 * Battery control and automatic shutdown
 * Control via serial console (DEBUG_SERIAL)
 * Dump settings (DEBUG_SERIAL)
 * Dump RFID card (DEBUG_SERIAL)
 * Sleep mode during playback (playback will be paused after a defined time period)
