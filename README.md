# SpreadControlVC

SpreadControlVC is a GTA Vice City DLL/ASI plugin that controls player and NPC weapon spread, spray, and recoil-like final trace deviation.

The current `1.0.0` baseline is tuned for the original `gta-vc.exe` addresses used during development. It hooks the instant-hit trace path so spread stays active when aiming at targets, walls, distant geometry, or open sky.

## Files

- `SpreadControlVC.cpp` - plugin source.
- `SpreadControlVC.ini` - runtime tuning values.
- `InjectSpreadControlVC.cpp` - simple DLL injector for `gta-vc.exe`.
- `build.bat` - x86 Visual C++ build script.

Generated binaries and logs are intentionally ignored by git.

## Build

Install Visual Studio with the x86 C++ toolchain, then run:

```bat
build.bat
```

This produces:

- `SpreadControlVC.asi`
- `SpreadControlVC.dll`
- `InjectSpreadControlVC.exe`

## Usage

For DLL injection:

1. Start GTA Vice City.
2. Run `InjectSpreadControlVC.exe`.
3. Check `SpreadControlVC.log` beside the DLL for patch status.

For ASI loading, copy `SpreadControlVC.asi` and `SpreadControlVC.ini` into the game folder and use an ASI loader compatible with GTA Vice City.

## Configuration

Edit `SpreadControlVC.ini`:

- `Accuracy` controls NPC spread input. Lower means more spread.
- `SpreadMultiplier` scales NPC spread.
- `PlayerSpread` changes the game's player spread constants.
- `SpreadDegrees` controls final trace angular spread.
- `MinSpreadUnits` keeps very close shots visibly non-pinpoint.
- `NoTargetDistance` is used when the game reports no hit target.
- `TraceLogShots` controls diagnostic trace logging. Set it to `0` after tuning.

## Version

Current baseline: `1.0.0`
