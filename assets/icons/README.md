# Icons (ROADMAP #34)

Not compiled into firmware — CLAUDE.md, "Что НЕ делать" is explicit that
assets stay off flash. Everything under this directory is a *build artifact
tracked for convenience*; the files that actually matter at runtime live on
the microSD card, copied there by hand (same workflow as `/sd/wifi.json`,
`/sd/agent.json`, `/sd/ota.json` — there is no on-device path that writes to
the card, so this is always a manual step).

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

Not done here, left to ROADMAP #35: a matching set of app-tile icons
(Full list grid, including Settings) — deliberately not picked yet, since
the right sizing/style call is easier to make once #35 is actually being
built, and the license question needs a properly open-licensed set (e.g.
Material Symbols, Apache 2.0), unlike the weather set above.
