# FFXIV-scdtool

Vibecoded now because I don't want to stare at Audacity for a long time and write ffmpeg `filter_complex`.

## Generating `.scd` files from Blu-Ray discs you own

- You need `ffmpeg`.
- `.flac` files should have their audio losslessly encoded from `.m2ts` files, and have IDv3 tags from `MP3DATA` copied.
- MHW: Have Windows Media Player set IDv3 tags automatically.
- Expected filesystem layout: `(any prefix)album name(any suffix)/(any prefix)(track name/track number/m2ts filename)(any suffix)`
   ```
   D:\Music\FFXIV OST\
       Before Meteor\Before_Meteor_FFXIV_###.flac <- MP3DATA filenames
       2.0 A Realm Reborn\#####.flac <- M2TS filenames
       2.5 - Before The Fall\61 - Primogenitor.flac <- English track title; track number optional
       3.0 - Heavensward\イマジネーション ～蒼天聖戦 魔科学研究所～.flac <- Japanese track title; track number optional
       The Far Edge of Fate\44 - Promises.flac <- Deathgaze Hollow fight in Dun Scaith
       ...
   ```
- Files requiring albums you don't have will be skipped. 

```
scdtool64 apply
  --game :global
  --ost "D:\Music\FFXIV OST"
  --preset presets
  --output-dir out
  [--dry-run]
  [--verify]
  [--audio-format flac]
  [--sampling-rate keep]
```

- `--game`: Path to the game installation. Use `:global` to look up global release installation automatically from Windows registry.
- `--ost`: Path to your copies of albums.
- `--preset`: Path to the [`presets`](https://github.com/Soreepeong/FFXIV-scdtool/tree/main/presets) folder containing JSON files.
- `--output-dir`: Output directory, path-combined with in-game paths like `out\music\ex1\BGM_EX1_Makosen_Field01.scd`.
- `--dry-run`: Don't actually perform the job.
- `--verify`: Re-read each written file and check that things check out.
- `--audio-format ogg[:-1~10]|ogg:lossless|flac[:0-8]|wav`: Only `ogg` and `ogg:lossless` are compliant. `flac` and `wav` requires XivAlexander's *Support Alternate Codecs for Musics*.
   - Size: ogg q10 < FLAC < ogg:lossless < WAV.
   - `pcm` is supported, but not for background musics.
- `--sampling-rate keep|44100|48000|96000|...`

## AI generated description

Mostly a side effect of making it figure out better presets than [this](https://github.com/Soreepeong/XivAlexander/tree/main/StaticData/MusicImportConfig).

> [!CAUTION]
> AI generated below.

### Checking the result

Score each built file against the game's own:

```
scdtool64 verify --game :global --built out --output scores.csv
```

The main column is `weighted`: a level-weighted log-mel similarity, where 1 is identical. verify
also checks the envelope, silence, the seconds before the loop end, head alignment and whether
the loop point clicks. Pass the presets and albums too, and it also judges every place where a
preset joins two pieces of recording together:

```
scdtool64 verify --game :global --built out --preset presets --ost "D:\Music\FFXIV OST" --spectrum --output scores.csv
```

`scdtool64 verify -h` explains each column.

To listen to one file side by side with the game's:

```
scdtool64 extract --game :global --input music/ex3/BGM_EX3_Town_Y_Bar.scd --output game.ogg
scdtool64 extract --input out\music\ex3\BGM_EX3_Town_Y_Bar.scd --output built.ogg
scdtool64 extract --game :global --input music/ex3/BGM_EX3_Town_Y_Bar.scd --loop-info
```

The last command prints the loop points and length as JSON. `apply --emit-original` also writes
each game file next to its replacement as `<name>.orig.scd`.

### Making a preset for music the presets don't cover

`match` compares game files against a folder of tracks and writes a preset for what it finds.
To look only at game music that no existing preset covers, one expansion at a time:

```
scdtool64 match --game :global --ost "D:\Music\Some Album" --discover --target-prefix music/ex5/ ^
    --exclude-preset "presets\Dawntrail.json,presets\Trail to the Heavens.json" ^
    --output new.json --summary-csv new.csv
```

- `--discover` takes the targets from the game's own BGM list. Without it, `--preset` supplies
  a list of targets to fill in.
- Each item gets a `matchInfo` with its score. What `match` could not decide is left with
  `"enable": false` and explained. The summary at the end, and `--summary-csv`, list those
  targets with the game's own names for them (zone, duty, Orchestrion title), ordered by how
  easy they are to settle.
- `--matched-only` writes only the confident matches, ready to use.

Then build from it like any other preset and check it:

```
scdtool64 apply --game :global --ost "D:\Music\Some Album" --preset new.json --output-dir out-new
scdtool64 verify --game :global --built out-new --preset new.json --ost "D:\Music\Some Album"
```

When a game file is an edit rather than a straight cut of the album track (a shortened intro,
a skipped repeat, two tracks joined), write the item by hand. The files in `presets/` are
examples of each case, and most items carry a `# comment` explaining themselves. A preset with
`searchDirectories` looks for its albums inside `--ost`, so give it the root folder of the
layout above, not the album folder. In short:

```json
{
  "name": "My presets",
  "searchDirectories": { "Some Album": { "default": true } },
  "items": [
    {
      "source": ["^01 ", "Track Title"],
      "target": {
        "path": "music/ex5/BGM_EX5_Example.scd",
        "segments": [
          { "sourceOffsets": { "source": { "offset": 1.52 } }, "length": 30.8 },
          { "sourceOffsets": { "source": { "offset": 50.0 } }, "crossfadeSeconds": 0.1, "crossfadeShape": "linear" }
        ]
      }
    }
  ]
}
```

| field | meaning |
|---|---|
| `source` | regular expressions matched against file names, tried in order until one names exactly one file. An object `{"A": [...], "B": [...]}` names several recordings for one item |
| `target.path` | game path of the `.scd`, or an array of paths that get the same audio |
| `segments` | played one after another. `offset` is where in the recording the segment starts, in seconds; `length` is how long it plays (the last segment runs to the end) |
| `crossfadeSeconds`, `crossfadeShape` | the crossfade into this segment. Equal-power unless `"linear"` |
| `fadeInSeconds`, `fadeOutSeconds` | fades of one segment, e.g. `fadeInSeconds: 0` to cut in at full level while the previous one fades out |
| `startSeconds` | place a segment at this time on top of the others instead of after them (layering) |
| `channels` | which source channel feeds each output channel, for multichannel game files |
| `sourceFilters` | an ffmpeg audio filter per source, e.g. `"volume=3dB"` |
| `driftPpm` | beside `offset`: speed correction for a recording whose clock runs slightly fast or slow |
| `"target"` | as a source name, the game's own audio, for stretches no album contains |
| `enable` | `false` keeps an item in the file without building it |

`apply` fits the offsets against the game's own file unless `--no-auto-offset` is given. It
also matches loudness, smooths the loop point and reproduces a fade-in that the game has and the
album does not. Each of these has an option to turn it off; see `scdtool64 apply -h`.

### Other commands

Put one audio file into an `.scd`, using a game file as the template:

```
scdtool64 toscd -t ":global::music/ex2/BGM_EX2_System_Title.scd" -i replacement.ogg -c ogg -oq 1.0 ^
    --loop-begin 1234 --loop-end 5.00 -o result.scd
```

Loop points are in samples (an integer) or in seconds (a decimal number).

Find which album track each clip of a Blu-ray soundtrack is:

```
scdtool64 match-disc --disc "E:\" --tracks "D:\Music\Some Album" --output clips.csv
```

Every command explains its options with `-h`.
