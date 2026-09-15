# Accessibility checks

Automated tests can't hear a screen reader, so run these by hand before a release
and after any UI change. Each check lists what should happen. Anything else is a
bug.

## Desktop app

Start it from a build with `./build/tether-gtk`.

### Keyboard only

Put the mouse away.

- [ ] Tab and Shift+Tab reach every button, switch, entry and list on every page.
      Focus is always visible.
- [ ] Ctrl+1 to Ctrl+5, Ctrl+N, Ctrl+F, Ctrl+, and Ctrl+Q do what the README table
      says. The main menu shows the shortcuts next to Settings and Quit.
- [ ] In the message box, Tab moves to Send. It does not type a tab.
- [ ] Escape and Ctrl+W close Settings.
- [ ] Arrow keys move through the device list and skip the WI-FI and BLUETOOTH
      headings.
- [ ] When Wi-Fi is down, the reason and the command that fixes it can be read and
      selected on the Devices welcome page.

### Pairing dialog

```sh
./build/tether-dialog --title "Pair?" --body "Test" --accept Accept --reject Reject; echo $?
```

- [ ] Reject has focus when the dialog opens. Enter prints `1`.
- [ ] Tab to Accept, then Enter prints `0`.
- [ ] Escape prints `1`.

### Orca

Start Orca with `orca`, or Super+Alt+S on GNOME.

- [ ] The header buttons are read as "Main menu" and "Look for devices".
- [ ] Each Settings switch and dropdown is read with its row title.
- [ ] The search boxes, the message box, "To:" and "Number to call" are read by name.
- [ ] Message bubbles start with "Sent:" or "Received:". Unread badges say
      "N unread messages".
- [ ] AirPods battery is read as "Left earbud 82%", not "L 82%". Call signal is read
      as "signal 3 of 5", not as block characters.
- [ ] Copy, Message and Dismiss buttons name the address or app they act on.
- [ ] The pairing dialog is announced with its title and body.

[Accerciser](https://gitlab.gnome.org/GNOME/accerciser) lists the accessibility tree.
No push button, toggle button or entry should have an empty name.

### Vision

- [ ] `GTK_THEME=HighContrast ./build/tether-gtk`: all text and focus outlines are
      readable.
- [ ] `gsettings set org.gnome.desktop.interface text-scaling-factor 1.5`: nothing is
      clipped or overlapping. Undo with `gsettings reset org.gnome.desktop.interface
      text-scaling-factor`.
- [ ] Secondary grey text is readable in both light and dark styles.

## iOS app

- [ ] Run `testIconButtonsHaveAccessibilityLabels` in `TetherUITests`.
- [ ] Xcode, then Open Developer Tool, then Accessibility Inspector: run Audit on
      each tab and on the share sheet.
- [ ] VoiceOver: every toolbar button has a name, chevrons and decorative icons are
      skipped, and each transfer and paired device reads as one item.
- [ ] Share a link from Safari with VoiceOver on. The result is spoken before the
      sheet closes.
- [ ] Settings, Accessibility, Motion, Reduce Motion: the status dot, the pairing ring
      and the share result icon do not animate.
- [ ] Settings, Accessibility, Display & Text Size, Larger Text at the largest size:
      text wraps and nothing is cut off.

## Browser extension

- [ ] On a page with a one-time code field, an autofilled code leaves focus in that
      field, and a screen reader reads the code.
- [ ] If you are typing in a different field when the code arrives, focus stays in
      that field.
