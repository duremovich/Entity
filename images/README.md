# Entity — Brand Assets

Drop-in brand asset bundle for the Entity media server.

## Contents

```
dist/
├── brand.html                          ← Full design canvas (offline, 4MB)
├── README.md                           ← this file
├── svg/
│   ├── mark.svg                        ← Primary mark (rounded plate)
│   ├── mark-square.svg                 ← Mark with square corners
│   ├── lockup-horizontal.svg           ← Mark + "entity" + "MEDIA SERVER" (horizontal)
│   ├── lockup-vertical.svg             ← Mark over wordmark (centered)
│   ├── lockup-stacked.svg              ← Mark + "entity" only (no tagline)
│   ├── lockup-horizontal-transparent.svg
│   ├── lockup-vertical-transparent.svg
│   ├── wordmark.svg                    ← "entity" wordmark only
│   └── wordmark-with-tagline.svg
└── png/
    ├── favicon-16.png
    ├── favicon-32.png
    ├── favicon-64.png
    ├── favicon-128.png
    ├── apple-touch-icon-180.png
    ├── icon-192.png                    ← PWA / Android
    ├── icon-256.png
    ├── icon-512.png                    ← PWA / Android
    ├── icon-1024.png                   ← App Store / iOS
    ├── mark-square-512.png
    ├── mark-square-1024.png
    ├── lockup-horizontal-2400.png
    ├── lockup-horizontal-1200.png
    ├── lockup-horizontal-1200-transparent.png
    ├── lockup-horizontal-mark-wordmark.png   ← no tagline
    ├── lockup-vertical-1000.png
    ├── lockup-vertical-1000-transparent.png
    ├── wordmark-2000.png
    ├── wordmark-with-tagline-2000.png
    ├── wordmark-1200-transparent.png
    ├── og-banner-1280x640.png          ← Twitter / generic social
    └── og-banner-1200x630.png          ← Open Graph / Facebook
```

## Colors

| Token         | Hex       | Use                                     |
| ------------- | --------- | --------------------------------------- |
| Plate         | `#3a1a5a` | Primary background plate inside the mark |
| Canvas        | `#1f0a35` | Dark canvas / page background; gridlines |
| Body (start)  | `#41b261` | Body cells gradient — top-left          |
| Body (end)    | `#5dff8a` | Body cells gradient — bottom-right      |
| Whites (start)| `#a8ffc4` | Eye whites gradient — top-left          |
| Whites (end)  | `#dfffe9` | Eye whites gradient — bottom-right      |
| Iris (start)  | `#ff3d0a` | Iris/accent gradient — top-left         |
| Iris (end)    | `#ffd86b` | Iris/accent gradient — bottom-right     |
| Accent        | `#ff7a2e` | Tagline / orange accent text            |
| Stroke        | `#3ca65a` | Subtle cell outline                     |

## Typography

**Wordmark / display:** Space Grotesk 600 (lowercase). Letter-spacing `-2.5%`.

**Tagline:** Space Grotesk 500, all caps, letter-spacing `+30%`.

## Usage

- **Favicon** — use `png/favicon-32.png` (HTML `<link rel="icon">`) or `svg/mark.svg` for a vector favicon.
- **App icon (web)** — `png/icon-192.png` and `png/icon-512.png` for the manifest.
- **App icon (iOS)** — `png/apple-touch-icon-180.png` for `<link rel="apple-touch-icon">`.
- **README hero / repo banner** — `png/lockup-horizontal-1200.png` (or `-transparent.png` if your README has its own background).
- **Open Graph / social cards** — `png/og-banner-1200x630.png`.
- **In-app brand mark** — `svg/mark.svg` is the canonical vector — scales cleanly to any size.

## Editing

The full design canvas is in `brand.html` — open it in any browser to browse every variant explored. The source for the asset generators lives in the design project. To regenerate this bundle, re-run the export from there.
