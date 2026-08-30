# Shader dumps

Drop the `.dxbc` files produced by the add-on's shader dumper here, then run the
**Disassemble shader dumps** workflow (or just push — it triggers on changes to `*.dxbc`).

CI runs `fxc /dumpbin` on each file and uploads the listings, plus a `SUMMARY.txt` collecting
every `dcl_*` line. Those declarations name each register the shader uses, which is how the
binding layout gets read directly rather than inferred.

To produce a dump, add to `ReShade.ini`:

```ini
[STRAYDLSS]
DumpShaders=1
```

Play until gameplay (the shader census rises from ~150 in the menu to ~728 in gameplay), then
collect `stray-dlss-shaders/` from the game directory.
