# Standalone OFD plugin for Okular

This project builds an external Okular generator for Open Fixed-layout
Documents (`.ofd`). Implementation lives in `src/`, tests in `tests/`, and
MIME/desktop/AppStream registration in `data/`. The `okular/` and `libofd/`
source trees are reference checkouts; building this plugin does not use or
modify them. The plugin links the installed `Okular::Core` SDK target.

## Build and install

Dependencies: CMake 3.22+, a C++20 compiler, Qt 6.8+ (Core, Gui, Xml, Widgets),
ECM and KF6 6.13+ (Archive, CoreAddons, I18n), and the Okular 6 development
package exporting `Okular6Config.cmake`, and `update-mime-database` from
shared-mime-info. Tests additionally require Qt Test and AppStream validation
tools when enabled by ECM.
QuaZip and libofd are not required.

```sh
cmake -S . -B /tmp/okular-ofd-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/tmp/okular-ofd-install
cmake --build /tmp/okular-ofd-build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/okular-ofd-build --output-on-failure
cmake --install /tmp/okular-ofd-build
. /tmp/okular-ofd-build/prefix.sh
okular /path/to/document.ofd
```

Direct installation refreshes the MIME database automatically. Fully exit and
restart Okular after installing: an existing process can cache the generator
list and MIME types. With `DESTDIR` packaging or cross-compilation, the refresh
is skipped; the package must arrange a MIME database update on the target.

If an earlier system installation reports that OFD is unsupported despite the
plugin being present, refresh the database and restart Okular:

```sh
sudo update-mime-database /usr/share/mime
```

Use the installation prefix's `share/mime` directory if it differs from `/usr`.

Use `-DBUILD_TESTING=OFF` for a plugin-only build. Use `CMAKE_PREFIX_PATH` or
`Okular6_DIR` to select a private SDK installation. The plugin must use the same
Okular Core ABI as the host application.

This workspace's installed SDK exports Debug Core ABI 5 alongside packaged
Core ABI 4. The system viewer uses ABI 4. CMake rejects SDK exports with
multiple ABIs unless an explicit configuration is selected. For this machine,
add this argument for **all build types**, including Debug:

```sh
-DOKULAR_OFD_CORE_CONFIGURATION=NOCONFIG
```

A normal, consistent SDK installation does not need that override. Compare
`ldd` on the installed generator and `okularpart.so` if uncertain: their
`libOkular6Core` SONAMEs must match. Loading both ABIs can abort with
`you need to call SettingsCore::instance before using`. Adding private settings
initialization to the plugin does not fix this ABI conflict. No changes to SDK
files or the Okular source are required.

## Reading and rendering

The backend follows `OFD.xml`, declared document/page locations, and resource
IDs. It reads ZIP entries in memory without extracting archive content to the
filesystem. The first document in a multi-document container is displayed.

Supported features:

- Declared page order and default/per-page physical sizes in millimetres.
- Public, document, page, and template resources, with isolated scopes.
- Qt-readable images, boundaries, affine transforms, and object opacity.
- Basic text with system or embedded outline fonts, baselines,
  `DeltaX`/`DeltaY` advances (including `g` repeats), size, bold, and italic.
  Embedded fonts are converted to paths without global font registration.
- Paths using `S`, `M`, `L`, `Q`, `B`, `A`, and `C`, fill rules, solid colors,
  affine transforms, and elliptical arcs rendered as cubic segments.
- Shared `DrawParam` resources and `Relative` inheritance, layer/group defaults,
  object overrides, line caps/joins, and positive-width dashed strokes.
- RGB, gray, and device CMYK colors; 1/2/4/8/16-bit channels, decimal/hex values,
  indexed palettes, document `DefaultCS`, and color alpha.
- Template pages and stable background/body/foreground layer ordering.
- Object clips using filled paths, text outlines, and positive-width strokes.
  Areas within a Clip are combined; successive Clips intersect. Area/shape
  transforms, boundaries, fill rules, and `TransFlag` are handled.

Unsupported features include custom glyph maps, advanced text layout,
synthetic bold/italic on embedded outlines, ICC color conversion, dashed
hairlines, gradients, patterns, nested clips, group transforms, composite
objects, stamps, annotations, attachments, and additional documents in the
same container. Unsupported features or font substitutions produce a warning.
The plugin does not verify signatures or provide text search/selection,
printing, or editing. It is not a complete OFD conformance implementation.

## Tests and format references

Tests generate small OFD archives in temporary directories and verify page
order/sizes, rendered pixels, fonts, drawing inheritance, template layering,
resource scopes, arcs, colors, clips, malformed inputs, and reopen/close behavior.
The integration test first installs to an isolated prefix and checks that the
installation generates the MIME database without a separate refresh command.
It loads the actual plugin through KPluginFactory using only installed public
Okular headers and verifies MIME discovery, metadata, pages, and errors.
It does not construct Okular's internal viewer or use private settings APIs.

`tests/data/OFDTest.ttf` has geometric glyphs that distinguish embedded fonts
from system substitutions. It is CC0-licensed and regenerates without external
font tools:

```sh
python3 tests/data/generate-font.py
```
