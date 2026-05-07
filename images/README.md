# entity — brand assets

Pixel-grid creature mark + lowercase wordmark for **entity**, a media server.

## What's here

```
dist/
├── README.md              ← you are here
├── brand.html             ← interactive design doc (open in browser)
├── svg/                   ← scalable vector source
│   ├── mark.svg                       primary mark, rounded corners
│   ├── mark-square.svg                primary mark, square corners (app icon)
│   ├── lockup-horizontal.svg          mark + entity + media server, side-by-side
│   ├── lockup-vertical.svg            mark above wordmark + tagline, centered
│   ├── lockup-stacked.svg             mark above entity (no tagline)
│   ├── wordmark.svg                   "entity" wordmark only
│   ├── wordmark-with-tagline.svg      "entity" + "media server"
│   ├── lockup-horizontal-transparent.svg
│   └── lockup-vertical-transparent.svg
└── png/
    ├── icon/              ← favicons & app icons (16 → 1024)
    ├── lockup/            ← rasterized lockups @ 1x and 2x
    └── social/            ← OG image, Twitter header, README hero
```

All SVGs are self-contained — text is converted to paths (Space Grotesk SemiBold / Medium) so they render identically without any font installed.

## Colors

| Token        | Hex       | Use                              |
|--------------|-----------|----------------------------------|
| canvas       | `#1f0a35` | page / canvas background         |
| plate        | `#3a1a5a` | mark plate inside the rim        |
| body-from    | `#41b261` | wordmark + body pixels (start)   |
| body-to      | `#5dff8a` | wordmark + body pixels (end)     |
| stroke       | `#3ca65a` | per-pixel cell stroke            |
| whites-from  | `#a8ffc4` | eye whites (start)               |
| whites-to    | `#dfffe9` | eye whites (end)                 |
| iris-from    | `#ff3d0a` | iris (start)                     |
| iris-to      | `#ffd86b` | iris (end)                       |
| accent       | `#ff7a2e` | "media server" / accent text     |

Gradients run top-left → bottom-right (135°).

## Type

- **Wordmark** — Space Grotesk SemiBold (600), lowercase, letter-spacing −3%
- **Tagline / accent** — Space Grotesk Medium (500), uppercase, tracking +30%

The exported SVGs convert text to paths, so no font install is required to render them.

## Favicon HTML

```html
<link rel="icon" type="image/svg+xml" href="/svg/mark.svg">
<link rel="icon" type="image/png" sizes="32x32" href="/png/icon/favicon-32.png">
<link rel="icon" type="image/png" sizes="16x16" href="/png/icon/favicon-16.png">
<link rel="apple-touch-icon" sizes="180x180" href="/png/icon/apple-touch-icon-180.png">
<link rel="manifest" href="/site.webmanifest">
```

## OG / social

```html
<meta property="og:image" content="https://yourdomain.com/png/social/og-1200x630.png">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:image" content="https://yourdomain.com/png/social/og-1200x630.png">
```
