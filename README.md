# Flowing Hair for RE_Kenshi

Flowing Hair adds cloth physics to compatible scalp hair and beards in Kenshi.
It includes integrated weather wind and tail support for The Cat Mod and Khajjit.
The native plugin runs under [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi)
and links against [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib).

## Build

Use Visual Studio with Visual C++ 2010 x64, Windows SDK 7.1, and the
[KenshiLib example dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps).
Place the dependencies beside this checkout as `KenshiLib_Examples_deps` or
set `KENSHILIB_DIR` and `BOOST_INCLUDE_PATH` to your dependency folders.

```powershell
.\build.ps1 -StageOnly
```

The script emits a Release x64 DLL and PDB
under `build/staged/`. The runtime target is Steam Kenshi 1.0.65 x64 with
RE_Kenshi 0.3.5 and KenshiLib 0.5.0. Place the DLL, `packaging/RE_Kenshi.json`
and `FlowingHair.ini` beside the matching Flowing Hair mod data.

The repository contains the plugin source, build files, default settings,
loader manifest.

## License

Flowing Hair source is licensed under [GNU GPL version 3 only](LICENSE).
External dependencies retain their respective licenses.
