# StraightLineDrag

StraightLineDrag is an experimental Electron app for macOS and Windows that shows a temporary overlay line while a global hotkey is held, then simulates a straight-line mouse drag when the hotkey is released.

The project uses Electron Forge for development and packaging, and `@electron/rebuild` for rebuilding the native addon against Electron.

Default hotkeys:

- Windows: `Ctrl + Alt + X`
- macOS: `Cmd + Option + X`
- While held, adding `Shift` snaps the line and final drag to 45-degree increments.

These defaults can be changed in `config/default.json`.

## Permissions

On macOS, the app needs Accessibility and Input Monitoring permissions to listen globally and simulate mouse input. Windows may also require elevated trust from security software because the app installs low-level input hooks and posts synthetic mouse events.

## Development

1. Run `npm install`
2. Run `npm start`
3. If the native addon needs a manual rebuild, run `npm run rebuild`

## Building

Run `npm run make`

Notes:

- Build DMG artifacts on macOS.
- Build Squirrel.Windows installers on Windows.
