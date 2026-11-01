# Third-party notices

## PhotoCraft 0.18.0

Film Dust Retouch 1.0 incorporates and adapts portions of the PhotoCraft 0.18.0
Rust codebase for its automatic Remove and Healing Brush processing.

The vendored PhotoCraft crates are located in `vendor/`:

- `vendor/algo`
- `vendor/raster`
- `vendor/geom`
- `vendor/color`
- `vendor/cms`

The automatic Remove implementation uses PhotoCraft's patch-based, non-local
inpainting implementation (`photocraft_algo::remove`) and supporting raster
code. The Rust healing module also contains an adaptation of PhotoCraft's
Poisson implementation. The PhotoCraft source is available at:

https://github.com/storytold/photocraft

PhotoCraft is dual-licensed under MIT or Apache-2.0, at your option. The
original license texts and upstream notices are preserved in
`third_party/photocraft/`:

- `third_party/photocraft/LICENSE-MIT`
- `third_party/photocraft/LICENSE-APACHE`
- `third_party/photocraft/NOTICE`
- `third_party/photocraft/ATTRIBUTION.md`

The upstream attribution file describes the wider PhotoCraft project. Film
Dust Retouch does not bundle the PhotoCraft application, its UI, fonts, icon
sets, or other assets listed there unless explicitly present elsewhere in
this repository.

## Other Rust dependencies

The Rust build also uses crates from crates.io. Their package metadata and
individual license information are available from crates.io and are resolved
by Cargo during the build. The dependency set is defined in `Cargo.toml` and
its generated `Cargo.lock` file.
