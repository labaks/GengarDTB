# Icons (ROADMAP #34, #35, #46)

Not compiled into firmware — CLAUDE.md, "Что НЕ делать" is explicit that
assets stay off flash. Everything under this directory is a *build artifact
tracked for convenience*; the files that actually matter at runtime live on
the microSD card. Getting them there no longer has to mean a physical card
swap: `components/fetch` (ROADMAP #34) reads `/sd/fetch.json` and downloads
whatever it points at straight onto the card over WiFi. That file itself is
still a one-time manual write (same as `/sd/wifi.json`/`/sd/agent.json`) —
but the recommended shape is `{"manifest_url":"http://<pc-ip>:8000/fetch.json.example"}`
(see that file below), so the *card* only gets touched once ever; every
later batch of icons is just re-serving an updated JSON file from the PC and
pressing Settings → Apps → "Fetch to SD card".

## weather/

Source: [google-weather-icons](https://github.com/mrdarrengriffin/google-weather-icons),
set-4 ("Google Weather Filled", 48×48 SVG, light theme).

⚠️ **Licensing note, by explicit user decision**: that repository ships no
LICENSE file and its own README says outright *"I do not own these icons.
All rights belong to Google"* — these are Google's own weather icons,
mirrored without a clear redistribution grant. This does not meet ROADMAP
#34's own original bar ("свободно лицензированный"/freely-licensed) for an
icon set. Decided to use them anyway for this personal, non-distributed
device. If this repository is ever made public or the icon files shared
onward, revisit this — do not assume the license question is settled.

Converted from SVG with `tools/icon_convert.py` (gradients flattened to a
flat fill first — MuPDF's SVG renderer does not resolve `<linearGradient>`
refs and silently falls back to black; see that script's own docstring) at
48×48, LVGL v9 `ARGB8888` binary format (12-byte header + raw BGRA pixels —
see `lv_image_dsc.h` for the exact layout).

**Destination on the card:** `/sd/icons/weather/<name>.bin` — copy this
whole folder's `.bin` files there. `main/builtin/weather.ui.jsonl`'s
`weather_icon` dict maps every Open-Meteo WMO weather code to one of these
16 files by absolute path; `components/widget/widget.c`'s `image_apply()`
was extended to accept an absolute `src` (leading `/`) specifically so a
*builtin* app (whose own directory is always on flash, `/fs/apps/weather`)
can still point at an SD-only icon — see that function's comment.

| WMO codes | icon |
|---|---|
| 0 | clear_day |
| 1 | mostly_clear_day |
| 2 | partly_cloudy_day |
| 3 | cloudy |
| 45, 48 | haze_fog_dust_smoke |
| 51, 53, 55 | drizzle |
| 56, 57, 66, 67 | icy |
| 61, 63 | rain_with_cloudy |
| 65, 82 | heavy_rain |
| 71, 73 | snow_with_cloudy |
| 75 | heavy_snow |
| 77 | flurries |
| 80, 81 | showers_rain |
| 85, 86 | scattered_snow_showers_day |
| 95 | isolated_thunderstorms |
| 96, 99 | thunderstorms |

## apps/

Full list tile icons + the Settings tile (ROADMAP #35). Source: [Material
Symbols](https://github.com/google/material-design-icons) (Apache 2.0 —
an actually open license, unlike weather/ above), outlined style, 48px
source SVGs, converted at 24×24 (see `tools/icon_convert.py` — same
pipeline, no gradients to flatten this time, these are plain single-color
glyphs). Recolored at render time to the shell's current text color
(`shell_theme_text()` in `components/shell/shell.c`), not baked in — found
on hardware that tinting them the same accent color as the LVGL theme
(`LV_PALETTE_DEEP_PURPLE`) made a focused tile's icon blend into its own
focus outline, which uses that same accent.

| App id | icon | Material Symbol |
|---|---|---|
| `clock` | clock face | `schedule` |
| `weather` | sun | `wb_sunny` |
| `hello` | waving hand | `waving_hand` |
| `system` | speedometer | `speed` |
| Settings tile | gear | `settings` |

**Destination on the card:** `/sd/icons/apps/<app id>.bin` for a *builtin*
app (its own directory is always `/fs/apps/<id>`, on flash — see
`shell.c`'s `tile_icon_path()`). A *user* app on `/sd/apps/<id>/` instead
keeps its icon right next to its own `ui.jsonl`, at `<dir>/icon.bin` — the
convention `docs/app-format.md` already documented before either #34 or #35
existed.

## logo.bin

Settings → About device (ROADMAP #46). Source: user-provided pixel-art
silhouette (`D:/Pictures/logo.png`, 31×27, flat black on transparent — not
part of this repo). Converted with `tools/icon_convert.py`'s PNG path at an
integer `scale` of 2 (nearest-neighbour, not the SVG path's `size_px` — see
that script's docstring for why raw pixel art needs an integer multiple
instead of a resize to an arbitrary square) → 62×54, fits the About screen's
64px logo row. Recolored at render time the same way as `apps/` above
(`shell_theme_text()`), so it stays visible in both themes instead of
vanishing as flat black on a dark background.

**Destination on the card:** `/sd/icons/logo.bin` — `settings.c`'s
`build_about()` falls back to the old placeholder circle if the file isn't
there, same degrade-not-crash rule as every other optional SD asset.

## fetch.json.example

One `{"items":[...]}` document covering every file above (both `weather/`
and `apps/`), destined for wherever `manifest_url` in the card's
`/sd/fetch.json` points — swap `<pc-ip>` for whatever this machine's LAN
address actually is and serve this directory with e.g.
`python -m http.server 8000 --bind 0.0.0.0` from here. ⚠️ **Bind explicitly to
`0.0.0.0`** — `python -m http.server` defaulting to `::` (IPv6-only) is what
made the very first fetch attempt fail with 16/16 items unreachable and an
empty server log; the device only ever has an IPv4 address.
