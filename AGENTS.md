@/home/leaeasy/.codex/RTK.md

# Repository Guidelines

## Goal and Source Boundaries

Build an independent **OFD (`.ofd`) reader plugin for Okular**. ODF/OpenDocument
is a different format and is outside scope. The user explicitly requires the
plugin to live in `src/` and forbids modifying the Okular source tree.

- `src/`: OFD parser, renderer, arc geometry, generator, embedded plugin JSON.
- `tests/`: Qt Test cases and generated-archive helpers; `tests/data/` contains
  a tiny geometric test font and its standalone regeneration script.
- `data/`: MIME package, desktop entries, and AppStream metadata.
- Root `CMakeLists.txt`: standalone project; imports the installed SDK through
  `find_package(Okular6)` and links `Okular::Core`.
- Root `PKGBUILD`: local-checkout Arch packaging; `makepkg -s` builds and tests
  without installing. Default build workspace is `.makepkg/`, preserving the
  real `src/` even during makepkg cleanup. Package staging uses `DESTDIR`.
- `okular/` and `libofd/`: reference checkouts, not plugin build inputs. Do not
  add plugin files or CMake registrations to `okular/`. Keep its Git tree clean.
- Environment metadata directories are not application modules. The workspace
  root is not currently a Git repository; the reference checkouts may be.

## Build and Verification

Requirements: CMake 3.22+, C++20, Qt 6.8+, ECM/KF6 6.13+, and an installed
Okular 6 SDK. Tests use Qt Test and `update-mime-database`. Read the root README
for supported features, limits, dependency selection, and run instructions.

Verified standalone build configuration for this environment:

```sh
cmake -S . -B /tmp/okular-ofd-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/okular-ofd-install -DOKULAR_OFD_CORE_CONFIGURATION=NOCONFIG
cmake --build /tmp/okular-ofd-build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/okular-ofd-build --output-on-failure
cmake --install /tmp/okular-ofd-build
. /tmp/okular-ofd-build/prefix.sh
```

The explicit SDK configuration selects the packaged Core ABI in this
machine's mixed SDK exports; ordinary SDK installations do not require it.
This applies to Debug builds too: the exported Debug library uses ABI 5,
while the system viewer uses ABI 4. CMake rejects mixed ABIs without an
explicit selection. Do not initialize private SettingsCore in the generator
to mask a conflict between multiple loaded Core libraries.
Always match the host application's Core ABI. Do not modify installed SDK
files to make the plugin build. Disable tests with `-DBUILD_TESTING=OFF` when
needed. Do not use previous in-tree build paths under `/tmp/kdegraphics-*`.

Direct installation refreshes the MIME database automatically. The integration
test installs to an isolated prefix and verifies generated MIME registration.
`DESTDIR` and cross-compiled installations skip refreshing the host database;
package managers must arrange an update on the target. Restart Okular after
installation to clear cached generator/MIME discovery.

On 2026-11-25 the independent Release build, both CTest entries (79 Qt Test
passes), private-prefix installation with automatic MIME update, and `DESTDIR`
staging without cache generation succeeded. Plugin-only
configuration also succeeded. The installed plugin and host use Core ABI 4.
The reference Okular Git tree was verified clean.

## Implementation Guidelines

Use installed public headers (`<okular/core/...>`). Do not include Okular's
private/generated headers such as `settings_core.h`, or rely on viewer-private
methods in external tests. Load the plugin through KPluginFactory for SDK
integration checks. Keep parsing/rendering tests independent of Okular internals.

Read OFD page order and resource IDs rather than equating resource images with
pages. The companion libofd reader only extracts image resources; it is not a
page renderer and is not linked by this project. Archive paths must stay inside
the container. Preserve bounds on decompression, geometry, nesting, and images.

Keep unsupported content observable through warnings and document limitations.
Do not claim full OFD support based on generated samples. Maintain isolated
page/template resource scopes. Embedded fonts must not be registered globally.

Match KDE C++ style and existing SPDX headers. Keep changes focused and avoid
unrelated formatting. The existing reference guide specifies clang-format 19;
use the installed ECM KDE style when formatting plugin sources.

## Tests and Reviews

Cover relevant rendered pixels, dimensions, resource references, valid edge
cases, malformed inputs, and reopening. Keep fixtures small and reproducible.
Run the independent tests after relevant changes and report actual results.
Do not report checks on old binaries as evidence after a failed build.

Before finishing, verify `git -C okular status --porcelain` is empty. Describe
supported behavior, remaining limitations, dependencies, and checks performed.
Use concise imperative commit subjects if commits are requested. Include
rendered examples for visible changes when useful.
