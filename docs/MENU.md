# On-screen menu (ELITE_RGB V4.20)

This page describes how the IR-remote menu is built and lists the full menu tree. The generated parts come from
`analysis/tools/menu_dump.py`, which reads the named decompile (`menu_handler`, 0x170BC) and the image.
Regenerate them with `cd analysis && python3 tools/menu_dump.py`.

## How it works

- `menu_items[192]` (0xDE42C, 0x58 bytes each) holds every entry. `menu_handler(1, 0xFFFF)` fills it once at
  start-up with constant stores. A submenu is a `menu_list` (20 bytes in .data):
  `{items, count, cursor, on_open, …}`, where `items` points to a slice of `menu_items`.
- Each field is either a value or a getter. One flag bit per field says "call `field(0)` instead of reading it".
  Items use this to hide themselves, change their limits or open a different submenu depending on other settings:

  | offset | field | getter flag | meaning |
  |---|---|---|---|
  | +0x00 | `type` | – | item type (`menu_type_ops`: enter, draw, key, leave, extra) |
  | +0x04 | `flags` | – | the flag bits in this table |
  | +0x08 / +0x0C | `label_short` / `label_long` | 0x1 | the long label is used when the panel is wider than 80 px |
  | +0x10 | `value` | 0x2 | pointer to the setting (`cfg_*` in symbols.tsv) |
  | +0x14 / +0x18 | `min` / `max` | 0x4 / 0x8 | 0x20000000 / 0x40000000: the field is a pointer to the limit |
  | +0x1C | `action` | 0x10 | the submenu `menu_list`; with flag 0x80000000 a handler function instead |
  | +0x20…+0x34 | colours | 0x20…0x200 | label fg/bg (red), value fg/bg when editable (green), read-only (yellow) |
  | +0x38 | `options` | 0x400 | `menu_options {names, levels, count}` for choice items |
  | +0x3C / +0x40 | `edit_level` / `view_level` | 0x1000 / 0x2000 | access level needed to change / to see the item |
  | +0x44 | `type_arg` | 0x4000 | handed to the type on enter |
  | +0x48 | `hooks` | – | optional callbacks; `[1]` runs when the item is left |
  | +0x4C | `on_draw` | – | called before drawing |
  | +0x50 | `config_key` | 0x8000 | NVRAM config name, e.g. `"Pool,%d,Label Effect"` |
  | +0x54 | `key_format` | – | formats the `%d` of the key (the level/pool being edited) |

- Access levels: the PIN typed on the remote sets the level (see ANALYSIS.md, *Display state machine and menu
  access*). Level 1 is user, 2 is setup and 3 is master. `menu_next_visible` skips items whose `view_level` is
  above the current level. An `edit_level` of 4 probably means nobody can change the item (read-only counters),
  since no level 4 has been seen.
- Per-level and per-pool settings use an edit copy. A submenu's `on_open` copies record *n* into slot 0
  (`menu_level_open`, `menu_pool_open`, `menu_textline_open`), and the items always point at slot 0.
- Item types (`menu_types`, 0xBDA68) are bar (volume sliders), char, effect, readout, text_b, choice,
  t6 (used only by the version line), number, number_or_unset ("Not Set", for passwords and delays) and
  ranges (RNG). `menu_types_text` adds the download status lines and text strings.
- "Clear Download" asks for a random 4-digit confirm code before the confirm submenu opens.
  `menu_download_confirm_open` picks it from 1000..9999.
- Protocol numbers, as `menu_protocol_submenu` and `menu_protocol_open` read them (`cfg_protocol_menu`), are
  1 Jupiters, 2 CCOM, 3 Translux, 4 Mikohn CON2, 5 Casino Link, 6 TGSNCP, 8 Cougar, 9 Bally DIU and 10 WMS.
  7 Turbo, 11 XPCL, 12 DPRO and 0 (none) have no protocol submenu. The order matches `PROTOCOLS` in
  studio/core.py.

## Tree

`submenu<-f` means the getter `f` picks the submenu at run time, and the tree shows every list it can return.
For choices, `max=DAT_…` is the option count read from .data.

```
menu_list_main (the root menu):
  [  1] menu_show_version_maybe      type=t6 edit_level=3 view_level=1
    [ 17] 'Serial No:'                 type=text edit_level=4 view_level=1 max=10
    [ 18] 'Theme:'                     type=choice config_key='Theme,Enabled' edit_level=2 view_level=2 max=DAT_000bd9e4
    [ 19] 'Storage:'                   type=text edit_level=3 view_level=1 max=10
    [ 20] 'Remote Feedback:'           type=choice config_key='Remote Feedback,Enabled' edit_level=2 view_level=1 max=DAT_000bd9e4
    [ 21] menu_show_app_source         type=choice edit_level=2 view_level=1 max=DAT_000bda50
    [ 22] 'Mem Avail:'                 type=readout edit_level=3 view_level=3 max=0xffffffff
    [ 23] 'Mem Used:'                  type=readout edit_level=3 view_level=3 max=0xffffffff
    [ 24] 'RNG Count:'                 type=readout edit_level=3 view_level=3 max=0xffffffff
    [ 25] 'Boot Count:'                type=number edit_level=2 view_level=2 max=0xffffffff
    [ 26] 'Auto-Diag:'                 type=choice edit_level=2 view_level=2 max=DAT_000bd9e4
    [ 27] 'RS232 Diags:'               type=text edit_level=4 view_level=2 max=6
      [171] 'Overrun Errors:'            type=number edit_level=4 view_level=2 max=0xffffffff
      [172] 'Break Errors:'              type=number edit_level=4 view_level=2 max=0xffffffff
      [173] 'Frame Errors:'              type=number edit_level=4 view_level=2 max=0xffffffff
      [174] 'Parity Errors:'             type=number edit_level=4 view_level=2 max=0xffffffff
    [ 28] 'RS485 Diags:'               type=text edit_level=4 view_level=2 max=6
      [175] 'Overrun Errors:'            type=number edit_level=4 view_level=2 max=0xffffffff
      [176] 'Break Errors:'              type=number edit_level=4 view_level=2 max=0xffffffff
      [177] 'Frame Errors:'              type=number edit_level=4 view_level=2 max=0xffffffff
      [178] 'Parity Errors:'             type=number edit_level=4 view_level=2 max=0xffffffff
      [179] 'Bytes Received:'            type=number edit_level=4 view_level=2 max=0xffffffff
      [180] 'Bytes Sent:'                type=number edit_level=4 view_level=2 max=0xffffffff
      [181] 'State Mach Errs:'           type=number edit_level=4 view_level=2 max=0xffffffff
      [182] 'Messages:'                  type=number edit_level=4 view_level=2 max=0xffffffff
      [183] 'Other Messages:'            type=number edit_level=4 view_level=2 max=0xffffffff
      [184] 'Quick Polls:'               type=number edit_level=4 view_level=2 max=0xffffffff
      [185] 'Poll Replies:'              type=number edit_level=4 view_level=2 max=0xffffffff
    [ 29] 'View LFS:'                  type=text edit_level=4 view_level=2 max=2 handler=menu_icons_load
  [  2] 'Display Number:'            type=number config_key='Display Number' edit_level=1 view_level=1 max=999
    [ 15] 'First Sync Disp#:'          type=number config_key='First Sync Display Number' edit_level=1 view_level=1 max=999
    [ 16] 'Last Sync Disp#:'           type=number config_key='Last Sync Display Number' edit_level=1 view_level=1 max=999
  [  3] menu_show_rng_version_maybe  type=choice config_key='RNG,Enabled' edit_level=1 view_level=1 max=DAT_000bd9e4 submenu<-menu_rng_submenu
    [143] 'Range(s):'                  type=ranges config_key='RNG,Range' edit_level=1 view_level=1
    [144] 'Same No.:'                  type=choice config_key='RNG,Allow Same Number' edit_level=1 view_level=1 max=DAT_000bda20
    [145] 'RNG Effect:'                type=effect config_key='RNG,Effect' edit_level=1 view_level=1 submenu<-menu_rng_effect_submenu
      [188] 'Outline Style'              type=choice edit_level=1 view_level=1 max=DAT_000bda44 submenu<-menu_outline_c_submenu
        [191] 'Outline Colour'             type=choice edit_level=1 view_level=1 max=menu_outline_colour_max
  [  4] 'Download Menu:'             type=choice config_key='Download,Menu' edit_level=2 view_level=menu_download_view_level max=DAT_000bda34
    [156] &DAT_000e5579                type=download_line edit_level=1 view_level=1 max=&DAT_000e5594
      [169] &UNK_000e55a9                type=download_line edit_level=3 view_level=1 max=&UNK_000e55c4
      [170] &DAT_000e55d9                type=download_line edit_level=3 view_level=1 max=&UNK_000e55f4
    [157] 'Download Mode:'             type=choice config_key='Download,Mode' edit_level=2 view_level=1 max=DAT_000bda30 submenu<-menu_download_mode_submenu
      [162] 'Data Rate:'                 type=choice config_key='Download,Modem,Data Rate' edit_level=2 view_level=1 max=DAT_000bda24
      [163] 'Init Command:'              type=text config_key='Download,Modem,Init Command' edit_level=2 view_level=1 max=0x40
      [164] 'Dial Command:'              type=text config_key='Download,Modem,Dial Command' edit_level=2 view_level=1 max=0x40
      [165] 'Dial Timeout:'              type=number config_key='Download,Modem,Dial Timeout' edit_level=2 view_level=1 max=0xf0
      [161] 'DataRate:'                  type=choice config_key='Download,Direct,Data Rate' edit_level=2 view_level=1 max=DAT_000bda24
      [166] 'Dial Command:'              type=text config_key='Download,Modem,Dial Command' edit_level=2 view_level=1 max=0x40
      [167] 'Dial Timeout:'              type=number config_key='Download,Modem,Dial Timeout' edit_level=2 view_level=1 max=0xf0
      [168] 'Site ID:'                   type=number config_key='Download,IMSynch,Site ID' edit_level=2 view_level=1 max=0xffff
    [158] 'Clear Download'             type=choice edit_level=2 view_level=1 max=DAT_000bd9e4 submenu<-menu_clear_download_submenu
      [159] menu_confirm_prompt_maybe    type=number edit_level=2 view_level=1 max=9999 submenu<-menu_download_code_submenu
        [160] &DAT_000e5579                type=download_line edit_level=1 view_level=1 max=&DAT_000e5594
  [  5] 'Audio:'                     type=choice config_key='Audio,Enabled' edit_level=menu_audio_level view_level=menu_audio_level max=DAT_000bd9e4 submenu<-menu_audio_submenu
    [146] 'Mode:'                      type=choice config_key='Audio,Mode' edit_level=1 view_level=1 max=DAT_000bd9f0
    [147] 'Master Vol.:'               type=bar config_key='Audio,Master Volume' edit_level=1 view_level=1 max=0x1f submenu<-menu_audio_mixer_submenu
      [150] 'Int. Vol.:'                 type=bar config_key='Audio Mixer,Internal Volume' edit_level=1 view_level=1 max=0x1f
      [151] 'Ext. Vol.:'                 type=bar config_key='Audio Mixer,External Volume' edit_level=1 view_level=1 max=0x1f
      [152] 'Midi Vol.:'                 type=bar config_key='Audio Mixer,Midi Volume' edit_level=1 view_level=1 max=0x1f
    [148] 'JPHit Vol.:'                type=bar config_key='Audio,Jackpot Hit Volume' edit_level=1 view_level=1 max=0x1f
    [149] 'Attract Vol.:'              type=bar config_key='Audio,Attract Volume' edit_level=1 view_level=1 max=0x1f
  [  6] 'Text Line:'                 type=number edit_level=1 view_level=1 max=0x10
    [139] 'Setup:'                     type=choice config_key='Textline,%d,Setup' edit_level=1 view_level=1 max=DAT_000bda18
    [140] 'Text Effect:'               type=effect config_key='Textline,%d,Effect' edit_level=1 view_level=1 submenu<-menu_textline_effect_submenu
      [187] 'Outline Style'              type=choice edit_level=1 view_level=1 max=DAT_000bda44 submenu<-menu_outline_b_submenu
        [190] 'Outline Colour'             type=choice edit_level=1 view_level=1 max=menu_outline_colour_max
    [141] 'Text:'                      type=text config_key='Textline,%d,Text' edit_level=1 view_level=1 max=0xff
  [  7] 'Attract Interval:'          type=number config_key='Attract,Time' edit_level=1 view_level=1 max=300
    [153] 'Setup Attract:'             type=number edit_level=1 view_level=1 max=0x19
      [154] 'Play:'                      type=choice config_key='Attract,%d,Play' edit_level=1 view_level=1 max=DAT_000bd9e8
      [155] 'Sound:'                     type=choice config_key='Attract,%d,Sound' edit_level=menu_attract_sound_level view_level=menu_attract_sound_level max=DAT_000bd9e4
  [  8] 'Jackpot Levels:'            type=number config_key='Jackpot,Levels' edit_level=2 view_level=1 max=menu_jackpot_levels_max submenu<-menu_jackpot_levels_submenu
    [ 73] 'Configure Jackpot:'         type=choice edit_level=1 view_level=1 max=menu_level_choice_count submenu<-menu_jackpot_submenu
      [ 74] 'Machine #:'                 type=number config_key='Jackpot,Machine #' edit_level=2 view_level=1 max=0xffffffff
        [ 83] 'Number Format:'             type=choice config_key='Jackpot,Machine No,Format' edit_level=2 view_level=1 max=DAT_000bda04
      [ 75] 'Location #:'                type=number config_key='Jackpot,Location #' edit_level=2 view_level=1 max=0xffffffff
      [ 76] 'Win When:'                  type=choice config_key='Jackpot,Win When' edit_level=2 view_level=1 max=DAT_000bda08
      [ 77] 'Odometers Shown:'           type=choice config_key='Jackpot,Odometers Shown' edit_level=2 view_level=1 max=DAT_000bda0c
      [ 78] 'Relay Polarities:'          type=choice config_key='Jackpot,Relay States' edit_level=2 view_level=1 max=DAT_000bda38
      [ 79] 'Link Idle:'                 type=number_or_unset config_key='Jackpot,Link Idle' edit_level=2 view_level=1 max=0xffff
      [ 80] 'Comms Idle:'                type=number_or_unset config_key='Jackpot,Comms Idle' edit_level=2 view_level=1 max=0xffff
      [ 81] 'Display ID:'                type=number edit_level=2 view_level=1 max=0x3f
        [138] 'Alt Description:'           type=text config_key='Jackpot,DispID,%d,AltDesc' edit_level=2 view_level=1 max=0x10
      [ 82] 'Max Win Time:'              type=number_or_unset config_key='Jackpot,Max Win Time' edit_level=2 view_level=1 max=9999
      [ 84] 'Setup:'                     type=choice config_key='Pool,%d,Setup' edit_level=2 view_level=1 max=DAT_000bd9f8
        [130] 'Odometer Style:'            type=choice config_key='Pool,%d,OdometerType' edit_level=1 view_level=1 max=DAT_000bd9fc
        [131] 'Odometer Font:'             type=choice config_key='Pool,%d,Odometer Font' edit_level=1 view_level=1 max=DAT_000bda00
        [132] 'Show Odometer:'             type=choice config_key='Pool,%d,Show Odometer' edit_level=2 view_level=1 max=DAT_000bd9e4
        [133] 'Multi-Speed Update:'        type=choice config_key='Pool,%d,Multi-Speed' edit_level=2 view_level=1 max=DAT_000bd9e4
        [134] 'Decimal Point Offset:'      type=number config_key='Pool,%d,Decimals' edit_level=2 view_level=1 max=2
        [135] 'Minimum:'                   type=number config_key='Pool,%d,Minimum' edit_level=2 view_level=1 max=&DAT_000e2d98
        [136] 'Maximum:'                   type=number config_key='Pool,%d,Maximum' edit_level=2 view_level=1 max=0xffffffff
      [ 85] 'Jackpot'                    type=text edit_level=4 view_level=2 max=9
        [116] 'Label:'                     type=text config_key='Pool,%d,Label' edit_level=2 view_level=1 max=10
        [117] 'Pre-Text:'                  type=text config_key='Pool,%d,Pre-Text' edit_level=2 view_level=1 max=10
        [118] 'Post-Text:'                 type=text config_key='Pool,%d,Post-Text' edit_level=2 view_level=1 max=10
        [119] 'Thousands Sep.:'            type=char config_key='Pool,%d,Thousands Separator' edit_level=2 view_level=1 max=1
        [120] 'Decimal Sep.:'              type=char config_key='Pool,%d,Decimal Separator' edit_level=2 view_level=1 max=1
        [121] 'Label Effect:'              type=effect config_key='Pool,%d,Label Effect' edit_level=1 view_level=1 max=DAT_000bda10 submenu<-menu_label_effect_submenu
          [186] 'Outline Style'              type=choice edit_level=1 view_level=1 max=DAT_000bda44 submenu<-menu_outline_a_submenu
            [189] 'Outline Colour'             type=choice edit_level=1 view_level=1 max=menu_outline_colour_max
        [122] 'Pre-Text Effect:'           type=effect config_key='Pool,%d,Pre-Text Effect' edit_level=1 view_level=1 max=DAT_000bda10
        [123] 'Post-Text Effect:'          type=effect config_key='Pool,%d,Post-Text Effect' edit_level=1 view_level=1 max=DAT_000bda10
        [124] 'Thou. Sep. Effect:'         type=effect config_key='Pool,%d,Thousands Separator Effect' edit_level=1 view_level=1 max=DAT_000bda10
        [125] 'Dec. Sep. Effect:'          type=effect config_key='Pool,%d,Decimal Separator Effect' edit_level=1 view_level=1 max=DAT_000bda10
        [126] 'Odometer Effect:'           type=effect config_key='Pool,%d,Odometer Effect' edit_level=1 view_level=1 max=DAT_000bda10 submenu<-menu_odometer_effect_submenu
        [127] 'Win Value Effect:'          type=effect config_key='Pool,%d,Win Effect' edit_level=1 view_level=1 max=DAT_000bda10 submenu<-menu_win_effect_submenu
        [128] 'Location Effect:'           type=effect config_key='Pool,%d,Location Effect' edit_level=1 view_level=1 max=DAT_000bda10 submenu<-menu_location_effect_submenu
        [129] 'New Jackpot Effect:'        type=effect config_key='Pool,%d,New Jackpot Effect' edit_level=1 view_level=1 max=DAT_000bda10 submenu<-menu_new_jackpot_effect_submenu
      [ 86] 'Speeds'                     type=choice config_key='Pool,%d,Speed Style' edit_level=2 view_level=1 max=DAT_000bda64 submenu<-menu_speed_style_submenu
        [ 97] 'Maximum Lag:'               type=number config_key='Pool,%d,Maximum Lag' edit_level=2 view_level=1 max=menu_pool_span
        [ 98] 'Lag Catchup:'               type=number config_key='Pool,%d,Lag Catchup' edit_level=2 view_level=1 max=&DAT_000e2d9c
        [ 99] 'Waypoint 0:'                type=number config_key='Pool,%d,Waypoint 0' edit_level=2 view_level=1 max=&DAT_000e2da8
        [100] 'Speed 0:'                   type=number config_key='Pool,%d,Speed 0' edit_level=2 view_level=1 max=&DAT_000e2dbc
        [101] 'Waypoint 1:'                type=number config_key='Pool,%d,Waypoint 1' edit_level=2 view_level=1 max=&DAT_000e2dac
        [102] 'Speed 1:'                   type=number config_key='Pool,%d,Speed 1' edit_level=2 view_level=1 max=&DAT_000e2dc0
        [103] 'Waypoint 2:'                type=number config_key='Pool,%d,Waypoint 2' edit_level=2 view_level=1 max=&DAT_000e2db0
        [104] 'Speed 2:'                   type=number config_key='Pool,%d,Speed 2' edit_level=2 view_level=1 max=&DAT_000e2dc4
        [105] 'Waypoint 3:'                type=number config_key='Pool,%d,Waypoint 3' edit_level=2 view_level=1 max=&DAT_000e2db4
        [106] 'Speed 3:'                   type=number config_key='Pool,%d,Speed 3' edit_level=2 view_level=1 max=&DAT_000e2dc8
        [107] 'Waypoint 4:'                type=number config_key='Pool,%d,Waypoint 4' edit_level=2 view_level=1 max=&DAT_000e2d9c
        [108] 'Speed 4:'                   type=number config_key='Pool,%d,Speed 4' edit_level=2 view_level=1 max=&DAT_000e2dcc
        [109] 'Max Speed:'                 type=number config_key='Pool,%d,Speed Max' edit_level=2 view_level=1 max=&UNK_003d0900
        [110] 'Maximum Lag:'               type=number config_key='Pool,%d,Maximum Lag' edit_level=2 view_level=1 max=menu_pool_span
        [111] 'Lag Catchup:'               type=number config_key='Pool,%d,Lag Catchup' edit_level=2 view_level=1 max=&DAT_000e2d9c
        [112] 'Min Point:'                 type=number config_key='Pool,%d,Waypoint 0' edit_level=2 view_level=1 max=&DAT_000e2db4
        [113] 'Min Speed:'                 type=number config_key='Pool,%d,Speed 0' edit_level=2 view_level=1 max=&DAT_000e2dc8
        [114] 'Max Point:'                 type=number config_key='Pool,%d,Waypoint 4' edit_level=2 view_level=1 max=&DAT_000e2d9c
        [115] 'Max Speed:'                 type=number config_key='Pool,%d,Speed Max' edit_level=2 view_level=1 max=&UNK_003d0900
      [ 87] 'Jackpot'                    type=text edit_level=4 view_level=2 max=0xb
        [ 88] 'PreWin Type:'               type=choice config_key='Pool,%d,PreWin Type' edit_level=2 view_level=1 max=DAT_000bda3c
        [ 89] 'PreWin Frame Count'         type=number config_key='Pool,%d,PreWin Frames' edit_level=2 view_level=1 max=99999
        [ 90] 'PreWin Start Delay'         type=number_or_unset config_key='Pool,%d,PreWin Fixed Start' edit_level=2 view_level=1 max=0xffff
        [ 91] 'PreWin Repeats:'            type=number config_key='Pool,%d,PreWin Repeats' edit_level=2 view_level=1 max=99
        [ 92] 'PreWin End Delay'           type=number_or_unset config_key='Pool,%d,PreWin End' edit_level=2 view_level=1 max=0xffff
        [ 93] 'NonWin Anim:'               type=choice config_key='Pool,%d,No Win Anim' edit_level=2 view_level=1 max=DAT_000bd9e4
        [ 94] 'Winning Machine No:'        type=choice config_key='Pool,%d,Show Winning Machine #' edit_level=2 view_level=1 max=DAT_000bd9e0
        [ 95] 'Label On Win:'              type=choice config_key='Pool,%d,Label on Win' edit_level=2 view_level=1 max=DAT_000bd9e4
        [ 96] 'Trigger On Win:'            type=choice config_key='Pool,%d,Trigger on Win' edit_level=2 view_level=1 max=DAT_000bd9ec
          [137] 'Relay Timeout:'             type=number config_key='Jackpot,Relay Timeout' edit_level=2 view_level=1 max=300
  [  9] 'Protocol:'                  type=choice config_key='Protocol' edit_level=2 view_level=1 max=DAT_000bd9dc submenu<-menu_protocol_submenu
    [ 30] 'Configure Protocol:'        type=choice edit_level=1 view_level=1 max=menu_level_choice_count submenu set by the list's on_open
      [ 31] 'Address:'                   type=choice config_key='Jupiters,Level,%d,Address' edit_level=2 view_level=1 max=0x100
      [ 32] 'Text Scroll Rate:'          type=number config_key='Jupiters,Level,%d,Text Rate' edit_level=2 view_level=1 max=0xff
      [ 33] 'Flash On Time:'             type=number config_key='Jupiters,Level,%d,Flash On' edit_level=2 view_level=1 max=&DAT_000018e7
      [ 34] 'Flash Off Time:'            type=number config_key='Jupiters,Level,%d,Flash Off' edit_level=2 view_level=1 max=&DAT_000018e7
      [ 35] 'Sound with Graphics:'       type=choice config_key='Jupiters,Level,%d,Graphic Sound' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 36] 'Broadcast Response:'        type=choice config_key='Jupiters,Level,%d,Broadcast Response' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 37] 'Comms Message:'             type=choice config_key='Jupiters,Level,%d,Comms Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 38] 'Odometer Jump:'             type=choice config_key='Jupiters,Level,%d,Odometer Jump' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 39] 'Odom. Speed Mesg.:'         type=choice config_key='Jupiters,Level,%d,Odometer Speed' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 40] 'Accept Decimal Mesg:'       type=choice config_key='Jupiters,Level,%d,Accept Decimal Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 41] 'Accept PreText Mesg:'       type=choice config_key='Jupiters,Level,%d,Accept Pre-Text Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 42] 'Accept PostText Mesg:'      type=choice config_key='Jupiters,Level,%d,Accept Post-Text Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 43] 'Accept Text Speed Mesg:'    type=choice config_key='Jupiters,Level,%d,Accept Text Speed Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 44] 'Accept Flash Rate Mesg:'    type=choice config_key='Jupiters,Level,%d,Accept Flash Rate Message' edit_level=2 view_level=1 max=DAT_000bd9e4
      [ 45] 'Display Type:'              type=choice config_key='Translux,Level,%d,Display Type' edit_level=2 view_level=1 max=DAT_000bda14
      [ 46] 'Display ID:'                type=choice config_key='Translux,Level,%d,Display ID' edit_level=2 view_level=1 max=0x40
      [ 47] 'Jackpot Level:'             type=choice config_key='Translux,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=DAT_000bd9d4
      [ 48] 'Group ID:'                  type=choice config_key='Translux,Level,%d,Group ID' edit_level=2 view_level=1 max=DAT_000bd9d4
      [ 55] 'Group:'                     type=number config_key='Mikohn CON2,Level,%d,Group Number' edit_level=2 view_level=1 max=0xff
      [ 56] 'Address:'                   type=number config_key='Mikohn CON2,Level,%d,Address' edit_level=2 view_level=1 max=0x40
      [ 57] 'Jackpot Level:'             type=number config_key='Mikohn CON2,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=8
      [ 58] 'Group:'                     type=number config_key='Casino Link,Level,%d,Group Number' edit_level=2 view_level=1 max=0xff
      [ 59] 'Jackpot Level:'             type=number config_key='Casino Link,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=8
      [ 49] 'Link ID:'                   type=number config_key='TGSNCP,Level,%d,Link ID' edit_level=2 view_level=1 max=0xffff
      [ 50] 'Jackpot Level:'             type=choice config_key='TGSNCP,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=DAT_000bd9d4
      [ 51] 'JNMS Level:'                type=choice config_key='Cougar,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=DAT_000bd9d4
      [ 52] 'Data Rate:'                 type=choice config_key='Cougar,Data Rate' edit_level=2 view_level=1 max=DAT_000bda24
      [ 53] 'Display ID:'                type=number edit_level=2 view_level=1 max=0x3f
        [ 54] menu_cougar_dispid_label     type=text_b config_key='Cougar,DispID,%d,Text' edit_level=2 view_level=1 max=0x10
    [ 60] 'Type:'                      type=choice config_key='CCOM Subtype' edit_level=2 view_level=1 max=DAT_000bda54 submenu<-menu_ccom_type_submenu
      [ 64] 'Configure Protocol:'        type=choice edit_level=1 view_level=1 max=menu_level_choice_count submenu<-menu_ccom_submenu
        [ 66] 'Prize ID:'                  type=number config_key='CCOM,Level,%d,Prize ID' edit_level=2 view_level=1 max=0xffff
        [ 65] 'Prize ID:'                  type=number config_key='CCOM,Level,%d,Prize ID' edit_level=2 view_level=1 max=0xffff
        [ 63] 'Prize '                     type=choice config_key='CCOM,Level,%d,Prize ID' edit_level=2 view_level=1 max=0xffff
    [ 61] 'Respond to Msgs:'           type=choice config_key='Bally DIU,Respond' edit_level=2 view_level=1 max=DAT_000bd9e4
    [ 62] 'Allow Reboot:'              type=choice config_key='Bally DIU,Allow Reboot' edit_level=2 view_level=1 max=DAT_000bd9e4
    [ 67] 'Configure Protocol:'        type=choice edit_level=1 view_level=1 max=menu_level_choice_count
      [ 71] 'WMS Level:'                 type=choice config_key='WMS,Level,%d,Jackpot Level' edit_level=2 view_level=1 max=0x10
      [ 72] 'Snap:'                      type=choice config_key='WMS,Level,%d,Snap' edit_level=2 view_level=1 max=DAT_000bd9e4
    [ 68] 'Address:'                   type=choice config_key='WMS,Address' edit_level=2 view_level=1 max=DAT_000bda5c
    [ 69] 'Allow Reboot:'              type=choice config_key='WMS,Allow Reboot' edit_level=2 view_level=1 max=DAT_000bd9e4
    [ 70] 'Display is :'               type=choice edit_level=2 view_level=1 max=DAT_000bda60
  [ 10] 'User Password:'             type=number_or_unset config_key='User Password' edit_level=1 view_level=1 max=9999
  [ 11] 'Setup Password:'            type=number_or_unset config_key='Setup Password' edit_level=2 view_level=2 max=9999
  [ 12] 'PAUSE:'                     type=choice edit_level=3 view_level=3 max=DAT_000bd9e4
  [ 13] 'RESET NVRAM:'               type=choice edit_level=3 view_level=3 max=DAT_000bda1c
  [ 14] 'DIAGNOSTICS:'               type=choice edit_level=3 view_level=3 max=DAT_000bda28 handler=diag_menu_dispatch
not reachable from the root by a static link:
[  0] 'Confirm Changes'            type=choice edit_level=1 view_level=1 max=DAT_000bda2c
[142] 'Reset Draw Table:'          type=choice edit_level=1 view_level=1 max=DAT_000bda1c
```
