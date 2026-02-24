# NVIDIA ShadowPlay DRM Bypass

Fixes ShadowPlay / Instant Replay being silently disabled when DRM sites (Spotify Web, Netflix, Kick, etc.) are open in your browser.

## The problem

Chrome/Edge's Widevine CDM process triggers NVIDIA's output protection, which disables screen capture entirely — even though you're not trying to capture DRM content. You get no warning, Instant Replay just stops working.

## How it works

Runs in the background and kills the browser's CDM utility process before it triggers the protection flag. Audio and video playback are unaffected.

## Usage

Run `nvbypass.exe` in a specific folder. A tray icon will appear in the system tray (bottom-right). Right-click it for options:

- **Show / Hide** — toggle the console window
- **Auto-start** — toggle Windows startup
- **Quit** — exit the program

| Command | Description |
|---|---|
| `nvbypass.exe` | Run with console visible |
| `nvbypass.exe --silent` | Run with console hidden |
| `nvbypass.exe --autostart` | Toggle Windows startup |

## Supported browsers

Chrome, Edge, Firefox, Opera, Brave, Vivaldi

## Building

Requires Visual Studio with C++ desktop workload.

```
cl /EHsc /O2 nvbypass.cpp /link /out:nvbypass.exe
```
