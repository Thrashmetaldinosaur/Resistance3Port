# Optional campaign Auto-Continue (Resistance 3)

This is an **opt-in automatic menu-navigation feature**, not an emulator savestate
or instant restore. It follows the game's normal start-up path, then selects
**Campaign > Continue** when a nonempty campaign save is present. It reduces
manual interaction, but does not eliminate engine initialization, shader
compilation, or checkpoint loading.

## Configuration

In the launcher: **Advanced > Start-up > Resume last campaign checkpoint**.

Alternatively, set the environment variable **R3_QUICK_RESUME=1**.
The default is off.

## Behavior

- Before activation, look for nonempty GAME.SAV and PARAM.SFO files in the
  selected PS3 savedata root for the US or EU disc title. This is a presence
  check, **not a savedata integrity or checksum verifier**.
- Allow shader compilation to complete. While waiting for the initial menu
  lobby, periodically send the standard confirm button to dismiss the start
  confirmation.
- Once the lobby has been observed, enter the main menu and Campaign, then
  select Continue using native pad input. The game itself performs the normal
  checkpoint load.
- On a missing save, send no inputs and leave startup normal. If the sequence
  cannot confirm a gameplay-level transition, stop automation without
  deleting or rewriting a save.
- The R3_BENCH=1 benchmarking mode disables this navigation for the launch.

## Limitations

The port does not currently expose a stable general-purpose Continue API, so
this version uses conservative input intervals after detecting the lobby.
It may not navigate a changed menu layout, every game language, or damaged
savedata correctly. It is not instantaneous resume, and slow menu transitions
may require retesting.

The presence check covers the primary campaign save location. A missing
checkpoint triggers normal startup, never New Game.

## Manual validation

Test using **copied** savedata directories, never the user's original files.
Checksums should be verified for the game installation and settings after each
isolated run.

- With a checkpoint present, confirm the quick-resume log shows startup,
  lobby detection, menu navigation and loading of a gameplay level. Then
  verify first-person gameplay with a visible HUD.
- Repeat with regular graphics settings and shader preloading enabled.
- With an empty copied save root, verify the "no checkpoint; normal startup"
  message and the absence of injected input.
- With the option disabled, confirm an ordinary interactive title screen.
- Check that the game's original saves and user settings remain unchanged.

No game-derived art, textures, executables, screenshots or private logs belong
in the contribution.
