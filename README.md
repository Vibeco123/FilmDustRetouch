# Film Dust Retouch

<img align="right" width="110" src="icon.png">

**A lightweight Windows application for frame-by-frame retouching of PNG image sequences.**

Film Dust Retouch is designed for cleaning animation frames, film scans, and other image sequences. It combines a native C++ Windows interface with a Rust image-processing library and PhotoCraft-based restoration and painting tools.

## Tools

### Spot Healing

Restore small defects using surrounding image content. Multiple dabs in one stroke are combined into a single mask and processed when the stroke ends.

### Remove

Automatically remove dust, spots, scratches, and other unwanted marks. Paint over the damaged area; the tool reconstructs it using nearby image content.

Based on PhotoCraft's patch-based non-local inpainting implementation, with loop closing, mask expansion, and gradient-domain blending.

### Healing Brush

Sample a source point with Alt + left-click, then paint using the sampled image content. The source is preserved independently of subsequent frame navigation, making it possible to copy details from another frame.

### Clone Stamp

Copy pixels directly from a sampled source point. Hold Alt and left-click to select the source. Unlike the Healing Brush, Clone Stamp does not automatically adapt colour or lighting.

### Pencil

A pixel-level drawing tool for precise manual retouching. Uses PhotoCraft's hard-edged Pencil engine, with strokes aligned to the pixel grid. Hold Alt to preview the pixel colour beneath the crosshair; Alt + left-click captures that colour for painting.

### Additional Tools

- **Blur** — locally blur image content.
- **Sharpen** — locally sharpen image details.
- **Smudge** — smear and blend image content.
- **Dodge** — lighten midtones.
- **Burn** — darken midtones.

## Controls

| Action | Control |
|---|---|
| Spot Healing | `1` |
| Remove | `2` |
| Healing Brush | `3` |
| Clone Stamp | `4` |
| Pencil | `5` |
| Blur | `6` |
| Sharpen | `7` |
| Smudge | `8` |
| Dodge | `9` |
| Burn | `0` |
| Paint with the active tool | Left mouse button |
| Sample source for Healing Brush / Clone Stamp | `Alt` + left-click |
| Preview / capture colour with Pencil | Hold `Alt` / `Alt` + left-click |
| Previous / next frame | `A` / `D` |
| Previous / next frame | Left / Right arrow |
| Fast frame seeking | Hold `Shift` while navigating |
| Go to frame by number or filename | `N` |
| Copy from previous frame | Hold `S` or Down arrow + left mouse button |
| Copy from next frame | Hold `W` or Up arrow + left mouse button |
| Undo | `Ctrl+Z` |
| Redo | `Ctrl+Shift+Z` |
| Zoom around cursor | Mouse wheel |
| Change brush size | `Shift` + mouse wheel |
| Pan image | Hold `Space` + drag, or middle mouse button + drag |
| Toggle zoom and pan lock between frames | `V` |
| Fit image to window | `F` |
| Toggle fullscreen | `Alt+Enter` |
| Exit fullscreen / fit image | `Esc` |
| Show information panel | `I` |
| Open another folder | `Ctrl+O` |
| Show current PNG in Explorer | `Ctrl+Shift+O` |
| Keyboard shortcuts and localized help | `F1` |
| Project support window | `F12` |

### Brush Settings

Right-click opens the settings for the active tool.

Brush size is shared across all tools. Hardness applies to soft-edged tools; Pencil always uses hard edges. Intensity is used by Blur, Sharpen, and Smudge, while Exposure is used by Dodge and Burn.

Settings are global and remain unchanged when switching tools.

### Frame Navigation

Tap `A` / `D` or the Left / Right arrow keys to move one frame. Holding a key starts playback at approximately 6 fps. Navigation loops at both ends of the sequence.

While holding a navigation key, press and release `Shift` to switch between normal playback and fast seeking at approximately 60 frame indices per second. During fast seeking, the last decoded frame remains visible; the selected target frame is loaded when the navigation key is released.

Decoded neighbouring frames are cached in RAM, up to three frames before and three frames after the current frame.

### Undo and Redo

Undo and Redo history is maintained separately for each frame. Up to eight Undo steps and the corresponding Redo history are retained while a frame remains in the RAM cache. One completed stroke creates one history entry.

### Saving

Modified PNG files are saved automatically when switching frames and when closing the application. Unmodified frames are not rewritten.

The application writes to a temporary file and replaces the original PNG after a successful write. Make a backup of your source sequence before the first retouching session.

## Requirements

- Windows 10 or later, 64-bit.
- Rust stable toolchain with the `x86_64-pc-windows-gnu` target.
- MinGW-w64 x86_64, including `g++`, `gcc`, `windres`, and `dlltool`.
- Cargo available in `PATH`.

The build script automatically adds `C:\mingw64\bin` to `PATH` if that directory exists. Otherwise, make sure your MinGW-w64 `bin` directory is already in `PATH`.

## Build from Source

1. Install Rust using [rustup](https://www.rust-lang.org/tools/install).
2. Install and select the GNU Rust toolchain:

   ```cmd
   rustup toolchain install stable-x86_64-pc-windows-gnu
   rustup default stable-x86_64-pc-windows-gnu
