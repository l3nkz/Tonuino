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
freeze dance, …) are not supported and also the admin mode is not yet fully
working. These are things that I will work on in the near future. However, the
basic features as for example reading the RFID card and playing the
corresponding files is fully functional. See the below list for a complete
overview about the finished features.

 * [ ] Button interaction
   * [x] 3 Button setup
   * [x] 5 Button setup
 * [ ] Play folder
   * [x] Album mode
   * [x] Playbook mode (will save progress in EEPROM)
   * [x] Repeat one mode
   * [x] Party mode
   * [ ] Album mode (von - bis)
   * [ ] Playbook mode (von - bis)
 * [ ] RFID cards
   * [x] Read RFID cards (same format as original code)
   * [x] Write RFID cards (same format as original code)
 * [ ] Admin mode
   * [x] Program new cards
   * [x] min/max volume
   * [ ] Pin code
   * [ ] Admin card
   * [ ] Math question to unlock
 * [ ] Settings
   * [x] Min/Max volume
   * [x] Last/initial volume
   * [x] Last active card/folder (this allows resuming the last playback after standby)
   * [x] Playbook progress
