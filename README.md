# Visual MediaPlayer

**Visual MediaPlayer** is a native 64-bit Windows application for browsing and playing local **videos, images, and VR media**.

It is built around three views:

- **Library** — visual browsing, folders, search, favorites, and resolution/VR badges.
- **Info** — large media banner, exact video metadata, and timeline previews.
- **Player** — hardware-accelerated flat and VR video playback.

Browsing data such as thumbnails and timeline previews is stored separately in a reusable cache.

**Platform:** Windows 10 / Windows 11, x64.

---

## Highlights

- GPU-backed Library and Info views using shared **Direct2D / DirectWrite** resources.
- **Direct3D 11 / DXGI** video playback.
- GDI fallback for Library/Info if the hardware renderer cannot be used.
- Videos, images, camera RAW files, and VR media in one Library.
- Video timeline previews with click-to-play navigation.
- Exact encoded video resolution shown in Info.
- VR180 / 360° playback with mouse-look and FOV control.
- Flat-video zoom from **0.25× to 8×** with drag-to-position.
- Image zoom, pan, slideshow, and Native Size.
- Favorites, search, folder navigation, and previous/next navigation.
- Fullscreen Library, Info, and Player modes.
- Always-on-top window toggle available in Library, Info, and Player.
- **Load Everything** for pre-generating reusable Library/Info cache data.
- Portable and Installer builds.
- Windows **Open with Visual MediaPlayer** integration for the installed edition.
- Automatic handling of removable/unavailable Library drives.

---

# Quick Start

1. Start **Visual MediaPlayer**.
2. Click the **Folder** button and select your media folder.
3. Choose **Videos** or **Images**.
4. Click a media card to open **Info**.
5. Click **Play** or press `Space` for video playback.
6. In Image Info, press `Space` to start or stop the slideshow.

The selected Library folder is remembered between launches. If its drive is unavailable, VMP keeps that Library selection and can use it again when the drive returns.

With the **installed edition**, you can also open supported files through File Explorer with **Open with Visual MediaPlayer**. Opening another file reuses the existing application window; opening several files together creates a temporary mini-library containing those files. The Portable build deliberately does not register shell associations.

---

# Controls

## Global

| Input | Action |
|---|---|
| `F11` | Toggle fullscreen |
| `Esc` | Context-sensitive Back / reset |
| Drag title bar | Move the application window |
| **Always on Top button** | Keep the VMP window above other normal windows until toggled off |

`Esc` resolves the current state first. For example, it resets flat-video zoom/pan before leaving Player and closes Library search before navigating back.

The **Always on Top** toggle is available in the Library, Info, and Player footers while windowed. It is hidden in fullscreen, where a separate topmost control is unnecessary. The active state uses the same bright toggled-button treatment as the other toggleable controls. Its state is preserved while entering/exiting fullscreen and while navigating between views until you toggle it off or close the application.

When Always on Top is enabled, controls respond to the first click even if VMP was not the foreground application. When Always on Top is disabled, the normal foreground-safety behavior remains: a first click can be consumed to activate VMP before the requested action is performed. Hover highlighting and hover previews are independent of that click safeguard and remain available whenever VMP is actually visible beneath the pointer.

## Library

| Input | Action |
|---|---|
| Type | Search the current folder |
| `Ctrl + F` | Toggle Favorite for media under the pointer |
| `Ctrl + A` | Select all search text |
| `Backspace` | Delete search characters |
| `Enter` | Open the first search result |
| `Esc` | Close search, go up one folder, or leave fullscreen at the Library root |
| Mouse Wheel | Scroll |
| `Ctrl + Mouse Wheel` | Resize Library cards |
| Click media | Open Info |

When media is opened from a search, previous/next navigation stays inside that filtered result set. Returning to Library preserves the search and current item.

The windowed Library footer orders the left-side controls as **Back → Always on Top → Video/Image toggle**.

Library scroll position is preserved when moving through Info/Player. Entering or leaving fullscreen preserves the **logical** scroll position as the number of cards per row changes; bottom stays bottom, top stays top, and intermediate positions remain approximately equivalent. Normal Library card sizing is also restored when leaving fullscreen even if fullscreen was exited from Info or Player, so fullscreen grid/card dimensions cannot leak into the windowed Library layout.

## Video Info

| Input | Action |
|---|---|
| `Space` | Play video |
| `Left Arrow` | Previous video |
| `Right Arrow` | Next video |
| Mouse Wheel | Scroll Info |
| `Ctrl + Mouse Wheel` | Resize timeline cards |
| Click timeline preview | Play from that timestamp |
| `Esc` | Return to Library |
| `F11` | Toggle fullscreen |

Timeline cards appear as their generated images become available. Selected Info/Timeline generation has foreground priority over **Load Everything**. Completed timeline frames remain reusable if batch generation yields, so an incomplete timeline can resume instead of restarting from zero.

The windowed Video Info footer orders the left-side controls as **Back → Always on Top → Play**. Timeline card sizing is restored to the saved windowed layout when leaving fullscreen, regardless of which view is active at the moment fullscreen is exited.

## Image Info

| Input | Action |
|---|---|
| `Space` | Start / stop slideshow |
| `Left Arrow` | Previous image |
| `Right Arrow` | Next image |
| Mouse Wheel | Zoom around the pointer |
| Left-click + drag while zoomed | Pan image |
| First `Esc` while zoomed | Reset to fit-to-window |
| Next `Esc` | Return to Library |
| `F11` | Toggle fullscreen |

Free image zoom/pan is disabled while **Native Size** is enabled.

---

# Flat Video Player

| Input | Action |
|---|---|
| `Space` | Play / Pause |
| `Left Arrow` | Seek backward 30 seconds |
| `Right Arrow` | Seek forward 30 seconds |
| Mouse Wheel | Zoom around the pointer |
| Left-click + drag on the video | Reposition the video |
| First `Esc` after zoom/pan | Reset to centered fit |
| Next `Esc` | Return to Info |
| `F11` | Toggle fullscreen |

Large side-arrow controls navigate to the previous/next video in the active navigation set.

### Zoom and positioning

Flat video can be zoomed from **0.25× to 8×** relative to fitted size.

- Above fit: drag to inspect cropped portions of the video.
- Below fit: drag the smaller video inside the available black surround.
- At fit: movement is available only on an axis with unused letterbox/pillarbox space.
- A drag can begin **only on actual rendered video pixels**.
- Dragging the black area surrounding the video does nothing.
- Once a valid drag starts, mouse capture keeps it continuous until release.
- `Esc` resets both zoom and position.

Flat-video zoom/pan is disabled while **Native Size** is enabled.

### Window movement and resizing

Player controls are separate owned overlay windows. While the main window is moved, the control popup and previous/next overlays follow it continuously instead of remaining at the old screen position and jumping afterward.

During interactive resizing, the current video surface remains stable and the swap-chain resize is deferred until the sizing operation finishes. This avoids repeatedly tearing down the video backbuffer for every intermediate mouse movement.

---

# VR Player

| Input | Action |
|---|---|
| Left-click + drag | Look around |
| Mouse Wheel | Change field of view |
| `Space` | Play / Pause |
| `Left Arrow` | Seek backward 30 seconds |
| `Right Arrow` | Seek forward 30 seconds |
| `Esc` | Return to Info |
| `F11` | Toggle fullscreen |
| **360° projection icon** | Toggle between front-only 180° and full 360° projection |

VR playback has its own mouse-look/FOV system. Flat-video zoom, flat-video positioning, and Native Size do not apply to VR playback.

Recognized VR layouts include:

- VR180
- 360° projection
- side-by-side stereo (`SBS`, `LR`)
- top/bottom stereo (`TB`, `OU`)
- mono panoramic video

## VR filename detection

Automatic VR classification uses an explicit **`VR` marker** in the filename. The recommended naming form is:

```text
Example VR.mp4
```

Numbered variants such as:

```text
Example VR (1).mp4
Example VR (2).mp4
```

remain grouped with the same VR name family when the Library sorts them.

Explicit 180° markers such as `VR180`, `180VR`, `VR 180`, and `180 VR` are also recognized. Layout markers such as `SBS`, `LR`, `TB`, and `OU` describe stereo packing.

The old `360` filename suffix is **not** a VR marker:

```text
Example 360.mp4  -> normal video
Example VR.mp4   -> VR video
```

This affects automatic classification only. The Player still supports both 180° and 360° projection modes.

---

# Video Resolution and Badges

VMP reads the encoded video frame size through **Windows Media Foundation**. The Info footer shows the exact encoded dimensions when available, for example:

```text
VR • 6144×3072
01:42:37
```

This is useful for VR files because Windows Explorer does not always display the dimensions for every codec/container.

The existing badge artwork is mapped by the largest encoded dimension:

| Encoded span | Badge |
|---:|---|
| 3840–5119 | 4K |
| 5120–7679 | 5K |
| 7680+ | 8K |

Therefore an intermediate VR resolution such as **6144×3072** uses the existing **5K** badge.

For stereo VR, the encoded frame can contain two eyes. Example:

```text
7680×3840 SBS
encoded frame: 7680×3840
per eye:       3840×3840
```

The Info footer intentionally reports the **encoded frame dimensions**.

---

# Search

Search applies to the folder currently displayed in Library. Start typing to search.

Favorited media also match the searchable term `favorite`, so `fav` can locate favorites while normal filename matching continues to work.

Special filters:

| Filter | Matches |
|---|---|
| `VR` | VR videos |
| `4K` | 4K-and-higher badge classes |
| `5K` | 5K-and-higher badge classes |
| `8K` | 8K badge class |

Examples:

```text
holiday 4k
concert vr
vacation 8k
```

Resolution metadata is cached after it has been determined for an unchanged file.

---

# Native Size

Native Size is available for **flat/non-VR videos** and **images**.

## Windowed flat video

Native Size is strict **1 source pixel = 1 screen pixel**:

- the window is resized for the source dimensions;
- it cannot be resized below the native dimensions;
- it may be enlarged beyond native size;
- enlargement does not upscale the video;
- an oversized native video is not silently reduced to fit the monitor.

## Fullscreen flat video

The application remains fullscreen and the video remains at strict 1:1 native pixel size. If the video is larger than the display, the oversized source may be clipped rather than scaled below native size.

## Windowed images

Native Size uses the image’s native dimensions and prevents the window from being resized below that native size. Enlarging the window does not upscale the image.

## Fullscreen images

Native Size keeps 1:1 rendering when the image fits. If the image is larger than the fullscreen client area, it is scaled down **only enough to fit completely inside the window**. Smaller images are never enlarged.

Leaving the media session resets Native Size for the next session.

---

# Auto Next, Slideshow, and Volume

## Auto Next

Auto Next starts the next video when the current video finishes. If Auto Next is enabled after a video has already reached its end, the next video starts immediately.

Auto Next resets to **Off** when leaving Player.

## Volume

A new video playback session starts at **30% volume**.

When Auto Next is active, a manually changed volume carries into the next automatically played video. Leaving Player resets the next new playback session to 30%.

Clicking the volume icon toggles mute. Mute is treated as an active/toggled state, so the button uses the same bright active-state treatment as other toggleable controls.

## Image slideshow

Press `Space` in Image Info to start or stop the slideshow. It advances through images in the current folder and resets when leaving the image session.

---

# Rendering Architecture

VMP uses the GPU where repeated composition benefits from it, while keeping file/codec work on the CPU.

## Library and Info

Library and Info share the same hardware **Direct2D / DirectWrite** rendering infrastructure for:

- Library cards and thumbnails
- Info hero/banner
- timeline cards and preview images
- text and timestamps
- VR / resolution / Favorite badges
- hover effects
- scrollable surfaces
- footer controls and UI composition

The existing GDI painters remain available as an emergency fallback if the shared hardware renderer cannot be created or must be recreated after a render-target/device failure.

## Player

Video playback uses **Direct3D 11 / DXGI**.

## CPU-side work

The following deliberately remain CPU-side because GPU composition does not make them faster:

- media/file scanning
- JPEG/RAW decoding
- Windows Media Foundation metadata probing
- cache generation/validation
- filesystem operations
- RAM cache management

---

# Cache and Memory

Generated browsing data is stored under a per-Library cache folder:

```text
.visualmediaplayer-cache
```

This includes reusable data such as:

- Library banners
- Info banners
- timeline preview JPEGs
- resolution/duration metadata

## Cache hierarchy

```text
disk cache
    ↓
decoded RAM bitmap cache
    ↓
temporary GPU bitmap cache
```

GPU copies are temporary working resources only; nothing GPU-specific is written to disk.

The decoded Library thumbnail cache has a nominal soft budget of roughly **640 MiB**. Under normal browsing pressure, VMP can discard cold/off-screen decoded copies and recreate them from the disk cache.

**Visible Library banners are protected UI state.** RAM pressure must not turn already-generated visible cards into permanent gray placeholders merely because the process is above its normal memory target. Existing disk-cache banners remain eligible to load for visible cards even when process RAM is elevated.

High-resolution and VR playback can use substantial dedicated/shared **GPU memory** for decoder surfaces and render resources. The size of a media file on disk is not its RAM requirement; video is streamed rather than loaded entirely into memory.

---

# Load Everything

**Load Everything** is available from the selected Library root. It pre-generates missing reusable data so later browsing requires less on-demand work.

For video this can include:

- Library banner
- Info banner
- timeline secondary images
- resolution metadata

Existing healthy cache files are reused rather than regenerated.

Large 8K/VR files can take significantly longer because Media Foundation may need to seek and decode very large frames repeatedly while producing cache artwork.

## Background priority and memory behavior

Load Everything is intentionally allowed to use more RAM than ordinary browsing. A single high-resolution/VR decode may legitimately use multiple GiB temporarily. The batch worker runs at **below-normal thread priority** so interactive work stays ahead of it.

The priority policy is:

1. **Playback and selected/visible previews** — highest priority.
2. **Visible Library thumbnails** — high priority and handled independently from the heavy batch gate.
3. **Load Everything** — low priority, but normally continues in the background.

When opening Player, VMP gives playback an approximately **700 ms** uncontested startup window while Media Foundation opens the source and establishes decoder/render resources. After that grace period, **Load Everything can continue during playback** at below-normal priority instead of remaining paused for the entire video.

Interactive navigation causes only short yields for heavy background generation. Library scrolling yields for roughly **80 ms**, while Details/Timeline scrolling and zooming use the general roughly **90 ms** background yield. Hover-preview startup gets an approximately **250 ms** uncontested decoder window; once started, its normal-priority decoder can run alongside the below-normal batch worker.

Selected **Info/Timeline generation always outranks Load Everything**. If foreground generation is requested while the batch owns the generation slot, Load Everything finishes its current atomic operation or timeline frame, releases ownership, waits for the foreground work, and then resumes. Timeline JPEGs already written remain reusable, so long timeline jobs can continue from completed work rather than throwing it away.

While Load Everything is active:

- its intentional decoder working set is not treated as ordinary Library memory pressure;
- visible/nearby Library banners are not blanked simply because process RAM rises;
- an open Info timeline is not purged just because another media file requires a large decode working set;
- the normal decoded-thumbnail LRU still limits unnecessary retained Library bitmaps;
- genuine machine-wide low-memory pressure can still hold back heavy batch work.

Timeline images generated for the currently open Info view are refreshed into the UI as they become available.

---

# Folder and Drive Availability

If the selected Library becomes unavailable, VMP unloads inaccessible Library/session state and shows:

> **This folder is unavailable.**

For drive-letter Libraries, VMP monitors whether the backing drive is mounted without repeatedly reading the media folder itself. This is useful for removable drives and mounted encrypted volumes.

If the saved Library later becomes available again, VMP can use it again automatically.

---

# Supported Media and Codecs

Recognizing a file extension means VMP will list/open the file; actual decoding still depends on the codecs available to Windows.

If a recognized file cannot be decoded, VMP shows:

> **This media is unsupported.**

## Video

Common recognized video extensions include:

`MP4`, `M4V`, `MKV`, `MK3D`, `WEBM`, `AVI`, `MOV`, `WMV`, `ASF`, `MPG`, `MPEG`, `TS`, `M2TS`, `VOB`, `OGV`, `FLV`, `F4V`, `3GP`, `RM`, `RMVB`, `MXF`, camera/elementary-stream formats, and related extensions.

Video decoding/playback uses **Windows Media Foundation**, so actual codec support depends on the Windows installation.

## Images and camera RAW

Recognized image formats include common raster formats plus many RAW extensions. Examples include:

`JPG`, `JPEG`, `PNG`, `APNG`, `BMP`, `GIF`, `TIFF`, `WEBP`, `HEIC`, `HEIF`, `AVIF`, `JXL`, `JPEG 2000`, `TGA`, `DDS`, `PSD`, `EXR`, `HDR`, `SVG`, `DNG`, `CR2`, `CR3`, `NEF`, `NRW`, `ARW`, `RAF`, `ORF`, `RW2`, `PEF`, `X3F`, and others.

Images use **GDI+** with a **Windows Imaging Component (WIC)** fallback. RAW decoding therefore requires a compatible WIC/Windows RAW codec. For example, Nikon `.NEF` files are recognized by VMP, but Windows must provide a decoder capable of opening that NEF variant.

---

# Fullscreen Behavior

Fullscreen is available in Library, Info, and Player.

- `F11` toggles fullscreen.
- Player → Info → Library can remain fullscreen throughout navigation.
- The **Always on Top** button is hidden while fullscreen and does not reserve a control slot.
- Library keeps its logical scroll location when fullscreen changes the grid width.
- Windowed Library and Timeline card sizes are saved on fullscreen entry and restored on fullscreen exit regardless of which view is currently active. This prevents fullscreen-sized cards or preview tiles from carrying over into the normal window.
- At the top-level Library, when no search/folder-navigation action remains, `Esc` exits fullscreen.
- `Esc` otherwise performs the current view’s Back/reset action before leaving fullscreen.

---

# Windows Explorer Integration

The Installer build registers supported media types for **Open with Visual MediaPlayer**.

From File Explorer:

1. Right-click a supported media file.
2. Choose **Open with**.
3. Select **Visual MediaPlayer**.

---

# Installer and Portable Builds

## Installer

The Installer build installs VMP under Program Files and can create/register:

- `VisualMediaPlayer.exe`
- Start Menu shortcut
- Windows **Open with** registration
- Installed Apps entry
- `Uninstall.exe`

The uninstaller removes application registration, settings, Start Menu entries, and the complete `C:\Program Files\Visual MediaPlayer` installation folder. Cache removal is optional.

To remove the installation folder immediately, the installed uninstaller first relaunches a temporary copy from `%TEMP%`, waits for the original `Uninstall.exe` to exit, and then removes the installed application tree. A reboot is only used as a last-resort fallback if Windows or another process is unexpectedly holding an installed file or directory open. The temporary helper itself may be scheduled for later cleanup from `%TEMP%`; it is not part of the installed application.

If the cache-removal checkbox is **not** selected, `.visualmediaplayer-cache` folders are left untouched.

## Portable

The Portable build requires no installation. Run:

```text
VisualMediaPlayer.exe
```

---

# Requirements

- 64-bit Windows 10 or Windows 11
- Direct3D 11-capable graphics hardware
- Windows Media Foundation
- Direct2D / DirectWrite
- GDI+
- Windows Imaging Component (WIC)
- compatible Windows codecs for the media being opened

No account, cloud service, browser, or internet connection is required for normal local playback.

---

# Building From Source

Visual MediaPlayer is a native **C++17 x64 Windows application**.

## Portable

```text
Portable\Build.bat
```

## Installer

```text
Installer\BuildInstaller.bat
```

Recommended build environment:

- Visual Studio / MSVC
- Windows SDK
- C++17
- x64 Release configuration

---

# Main Technologies

- C++17
- Win32 API
- Direct3D 11 / DXGI
- Direct2D / DirectWrite
- Windows Media Foundation
- GDI+
- Windows Imaging Component (WIC)
- Windows Shell APIs

---

**Visual MediaPlayer**

---

# Portable privacy and accessibility

The **Portable** build keeps its settings beside the executable in `VisualMediaPlayer.settings.ini` and does not register itself in the current user's `Software\\Classes` registry tree. It also uses a separate single-instance identity from the installed edition, so portable and installed copies can run independently. Library thumbnail/timeline cache folders can still be stored with the media library because they are reusable library data rather than Windows profile state.

Visual MediaPlayer exposes its custom-painted Library, Info and Player controls through Windows `IAccessible`/MSAA. Windows bridges this tree to UI Automation for compatible assistive technology. Visible media cards, timeline entries and the principal playback/navigation controls expose names, roles, states, screen locations, shortcuts and default actions.

# Optional code signing

Release build scripts support optional Authenticode signing through `tools\\SignRelease.ps1`. Configure either `VMP_SIGN_CERT_SHA1` for a certificate in the Windows certificate store or `VMP_SIGN_PFX` for a PFX file. `VMP_SIGN_PFX_PASSWORD` and `VMP_TIMESTAMP_URL` are optional. If no signing identity is configured, the scripts build normally and leave the output unsigned.
