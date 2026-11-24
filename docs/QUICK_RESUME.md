# Fast Start and Campaign Auto-Continue

These **optional** Windows launcher settings automate the game's existing frontend inputs. Neither mode bypasses engine initialization, installs game data, calls private guest functions, or restores a savestate. Both are **off by default**.

## Settings

In **Advanced > Start-up**:

- **Fast start to Campaign menu** (`R3_FAST_START=1`): confirms the game's startup install-data notice and advances through the title and main menu to the Campaign selection screen. It does **not** choose Continue, New Game, or Load Level.
- **Resume last campaign checkpoint** (`R3_QUICK_RESUME=1`): if an existing primary checkpoint is present, automates Campaign > Continue. If no checkpoint exists, Fast Start is used when enabled; otherwise normal startup remains available.

The settings can be turned off at any time. Disabling both restores the normal interactive title and menus. Benchmark mode keeps its independent scripted input flow and disables this startup automation.

## Implementation

The port first waits for shader compilation, then sends a short native controller confirmation starting at approximately 22 seconds from process start. When the frontend is slow, it retries at 2-second intervals, with a maximum of 25 attempts. Once the lobby level is detected, it advances via the normal frontend. Short conservative delays between menus allow their state to settle. The mode stops injecting input once the Campaign selector or a gameplay level is reached. If progress cannot be verified, it stops and leaves the menus interactive.

This **removes the need to manually confirm** the disk-install notice; the notice may still briefly appear on some hardware. No installation data is deleted or modified to suppress it. The port uses existing native controller injection, not keyboard macros or direct guest-memory patches.

Auto-Continue checks for nonempty `GAME.SAV` and `PARAM.SFO` in the primary campaign save folder for the US (`BCUS98176`) or EU (`BCES01118`) edition. This is a basic file-presence check, **not** PS3 savedata integrity validation. No save files are rewritten by the startup feature.

## Validation and limitations

Validation requires a real run of the game, as UI timing and renderer readiness vary with hardware, shader-cache state, game data and language. Test both settings with an isolated save root, confirm lobby-to-menu transitions in the runtime log, and visually inspect the final Campaign selection before shipping. Test a missing-save scenario and verify normal fallback. Preserve original saves and installed game data while testing.

The feature is an optimization of the *interactive startup flow*, not true direct level loading. Timings from a development machine should not be treated as guarantees. A genuine boot-to-checkpoint shortcut would require additional guest boot-state research and separate safety testing.
