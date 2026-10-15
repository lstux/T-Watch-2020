# Slovingo app

Reads a language course published by the [Slovingo](https://github.com/lstux/Slovingo)
engine and lets you read its sheets and practise on the watch.

## Data

The app needs the compact export of a course (`src/export_api.py`, branch `feat/t-watch`
of Slovingo), served over http(s) from a folder:

```
<url>/index.json            course title and list of sheets
<url>/sheets/<id>.json      one sheet (text, tables, dialogue cards)
<url>/exercises/<id>.json   the exercises of that sheet
```

Every file is downloaded on demand (one at a time, by a worker task) and cached in the SPIFFS,
so a course already opened keeps working without wifi. Refresh (bottom middle) forces a download.

## Configuration

`/slovingo.json` on the SPIFFS, see `conf/slovingo.json.example`. It can also be changed from
the app (setup button, bottom right), which opens the course list.

The first start shows "Choisis ton cours": a static list in `slovingo_config.cpp`
(`slovingo_courses[]`, the repositories `lstux/Slovingo-xx-xx*` published on GitHub Pages).
"Autre adresse..." at the end of the list keeps the free `url` field. Each course has its own
cache (the cache file is named after the url).

| key         | default                                                  | meaning                      |
|-------------|----------------------------------------------------------|------------------------------|
| `url`       | `https://lstux.github.io/Slovingo-sk-fr-kids/api/` | folder of the course export  |
| `chosen`    | `false`                                                  | a course was picked (otherwise the list is shown) |
| `questions` | `10`                                                     | questions per exercise round |
| `name`      | `Líška`                                                  | name of the player, shown where the course has `[USER_NAME]` |

## What it does

Series list -> sheets -> *read* the sheet, or play its exercises.

- **Reading** goes page by page, the arrows in the footer turn the pages. Text blocks are
  put together, dialogue lines are chat bubbles (the fox, i.e. you, on the right, the others on the left,
  coloured name) with the translation under the line and the word by word glosses on a tap, and vocabulary tables become cards
  (word, then sound and meaning; the translations start hidden, a tap shows or hides one, a button shows or hides all
  of them to test yourself).
- **Exercises**: multiple choice, fill-in-the-blank and word ordering (the words of the
  sentence plus one or two that do not belong, tap them in order). The next question comes
  by itself after an answer, the progress and score are in the footer.
- **Mascots**: the fox (the player) sits in the footer of the quiz and smiles or cries
  at the answers, the watch buzzes a little (when "vibe feedback" is on); the result
  screen cheers with a Slovak word, the fox, and the rabbit from 70 %.
- `[USER_NAME]` in the course is replaced by `name` of the config.
- **Standby**: while a quiz, the result or a sheet is shown, the watch does not jump back
  to the main tile when the screen goes off (the "block return to main tile" setting is
  restored when leaving). If it is left anyway, the series resumes where it was (an answered
  question moves on, an unanswered one starts again).

Not on the watch: audio (`listen` exercises), images and matching exercises.
Emojis and typographic symbols outside the font are dropped or replaced by ASCII
(`slovingo_sanitize()`); the emoji of a speaker is replaced by a name.

## Fonts, icon and mascots

The default fonts of the firmware have no `č ď ĺ ľ ň ô ŕ š ť ž`. This app ships its own
DejaVu Sans based fonts (Basic Latin, Latin-1 and Latin Extended-A, 16 and 22 px) in `font/`
and does not touch the global ones. `tools/gen_assets.py` regenerates fonts, icon and the
fox and rabbit sprites (needs Pillow and DejaVu Sans).

The app is built for every board except the small flash ones, where `support/apps_off.py`
leaves it out (`NOT_ESSENTIAL`).
