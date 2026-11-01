# Film Dust Retouch 1.1

**A lightweight Windows tool for retouching PNG frame sequences.**

Film Dust Retouch is designed for frame-by-frame cleanup of animation, film
scans, and other image sequences. It combines a native C++ Windows interface
with a Rust image-processing library.

## Tools

### Remove

Paint over dust, spots, scratches, or other unwanted marks. The tool
automatically completes the painted region using nearby image content. It is
based on PhotoCraft's patch-based non-local inpainting implementation, with
loop closing, mask expansion, and gradient-domain blending.

### Healing Brush

Choose a source point with the right mouse button, then paint with the left
mouse button. The brush transfers texture while adapting it to the destination
lighting and colour using Poisson healing.

### Clone Stamp

Choose a source point with the right mouse button, then paint with the left
mouse button. The stamp copies pixels directly, without automatic colour or
lighting adaptation.

## Controls

| Action | Control |
|---|---|
| Paint with the active tool | Left mouse button |
| Set source point for Healing Brush / Clone Stamp | Right mouse button |
| Remove | `1` |
| Clone Stamp | `2` |
| Healing Brush | `3` or `H` |
| Clear source point and return to Remove | `Esc` |
| Zoom around cursor | Mouse wheel |
| Change brush size | `Shift` + mouse wheel |
| Pan image | Middle mouse button + drag |
| Previous / next frame | Left / Right arrow |
| Copy from previous frame | Hold Down arrow |
| Copy from next frame | Hold Up arrow |
| Undo | `Ctrl+Z` |
| Redo | `Ctrl+Shift+Z` |
| Fit image to window | `F` |
| Save current frame | `Ctrl+S` |

## Requirements

- Windows 10 or later, 64-bit
- Rust stable toolchain: `x86_64-pc-windows-gnu`
- MinGW-w64 x86_64, including `g++`, `gcc`, and `windres`
- Cargo available in `PATH`

The build script automatically adds `C:\mingw64\bin` to `PATH` if that
folder exists. Otherwise, make sure your MinGW-w64 `bin` directory is already
in `PATH`.

## Build from source

1. Install Rust using [rustup](https://www.rust-lang.org/tools/install).
2. Install the GNU Rust toolchain:

   ```cmd
   rustup toolchain install stable-x86_64-pc-windows-gnu
   rustup default stable-x86_64-pc-windows-gnu
   ```

3. Install MinGW-w64 x86_64 and make sure `g++`, `gcc`, and `windres` are
   available in Command Prompt.
4. Open the project folder and run `build.bat`.
5. The build script compiles the Rust DLL and the C++ application. The output
   is placed in `publish\`:
   - `publish\FilmDustRetouch.exe`
   - `publish\film_dust_heal.dll`

On the first build, Cargo may download crates from crates.io. Keep the EXE and
DLL in the same folder when running the application.

## Usage notes

- Open a folder containing PNG frames.
- Work on copies of important source frames until you are comfortable with the
  workflow.
- Modified PNG frames are saved by the application; unmodified frames are not
  rewritten.
- Automatic Remove can take longer on large painted regions.

## Credits and licensing

Film Dust Retouch's original code is released under the MIT License. See
[`LICENSE`](LICENSE).

This project incorporates and adapts code from **PhotoCraft 0.18.0** under its
MIT OR Apache-2.0 licensing. The original license texts and notices are
preserved in [`third_party/photocraft/`](third_party/photocraft/). See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for details.

Film Dust Retouch is an independent project and is not affiliated with,
endorsed by, or sponsored by the PhotoCraft project or its contributors.

---

## Support the project

If you find Film Dust Retouch useful, you can support its development via Monobank.

[![Monobank QR Code](monobank-qr.png)](https://send.monobank.ua/jar/4uGrBkA4ub)

[Support via Monobank](https://send.monobank.ua/jar/4uGrBkA4ub)

## Author

**Vibeco**
