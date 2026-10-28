Pluto Bar
=========

A suckless status bar for Hyprland and river like DWM status bar.

Build
-----

    make

Run
---

    ./pluto-status | ./pluto-bar

`pluto-status` is the compiled status generator.  It is a drop-in
replacement for the legacy `status.sh`, which is kept in the tree as a
reference.  `pluto-status --once` prints a single line for testing.

Configuration is compile time, like dwm: edit `config.h` for the bar and
`status-config.h` for the status line, then run `make` again.  Status
modules have independent intervals; the defaults use 2-second updates for
CPU, RAM and brightness, 5 seconds for temperature and wifi, 10 seconds for
battery and home, 60 seconds for uptime and the clock, and 30 seconds for
Bluetooth and power-profile checks.  Set the intervals to `1` if you want
the old 1 Hz behaviour.

The status generator only writes when the text changes.  Each module has
its own interval in `status-config.h`; the scheduler sleeps until the next
module is due instead of reading every module every second.  The bar also
sleeps while a status pipe is connected; it keeps a one-second timer only
for its built-in clock fallback, and a five-second retry when a window
manager socket is disconnected.

Hardware that is absent at startup is not scheduled: battery, temperature,
backlight and power-profile modules are removed from the interval set, so a
short interval on missing hardware cannot cause wakeups.  Audio is polled
only when `wpctl` exists, or read from libpulse events when connected.  Wifi
keeps its normal off symbol, but rediscovery is limited to
`WIFI_RETRY_INTERVAL` seconds instead of sampling every `WIFI_INTERVAL`, so
a hotplugged interface still appears.  Interfaces are found through
`/proc/net/wireless` and `/sys/class/net`, so cfg80211 drivers that do not
populate the WEXT file are still detected; when the link is associated but
no signal level is exposed, the full-strength symbol is shown.  Bluetooth
keeps its off symbol when `bluetoothctl` is missing; no helper process runs
in that case.

Audio and music use event sources when available:

* `libpulse` is optional.  If `pkg-config` finds it, `pluto-status` follows
  the default sink and source through PulseAudio/PipeWire-Pulse, so volume
  and microphone changes wake the status immediately without periodic helper
  processes.  Without it the build still works and falls back to `wpctl`
  polling at `AUDIO_INTERVAL`.  If you install or remove the development
  package, run `make clean && make` so the status object is rebuilt against
  the new flags.
* `playerctl` is optional.  If it is installed, one persistent
  `playerctl --follow` process replaces polling, with an occasional liveness
  check while a song is cached.  Without it the song module is disabled.

CPU, RAM, home, uptime, temperature, battery, wifi, brightness and the
clock are read directly from `/proc`, `/sys`, or the kernel.  Their refresh
rates are independent; slow-changing modules default to longer intervals.
Bluetooth and power-profile helpers are queried infrequently and cached when
their hardware or helper exists.
