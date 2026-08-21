# Audio Device Switcher (Tray Icon)

**DISCLAIMER** This was created using Claude. It is not 100% ai, but about 20% ai

Adds a small speaker icon to the notification area. Click it (left- or
right-click) to open a lightweight popup menu listing every currently
**active** Playback (output) and Recording (input) device, grouped, with
the current default marked with a bullet. Pick a device to make it the new
default immediately, across all roles (Console, Multimedia, and
Communications).

Optional features (toggleable in the mod's settings):
- **Live device-change detection**: the tray tooltip stays in sync
  automatically when devices are plugged/unplugged or the default changes
  from elsewhere (Settings, another app).
- **Keyboard shortcuts**: `Ctrl+Alt+O` cycles to the next active output
  device, `Ctrl+Alt+I` cycles to the next active input device, with a
  balloon notification confirming the switch.

This relies on the same undocumented `IPolicyConfig` COM interface that
Windows' own Sound control panel and third-party utilities (EarTrumpet,
SoundSwitch, NirCmd, ...) use to change the default audio device, since
Microsoft has never shipped a public API for it. It has been stable since
Windows 7, but as with anything undocumented it could theoretically change
in a future Windows update.
