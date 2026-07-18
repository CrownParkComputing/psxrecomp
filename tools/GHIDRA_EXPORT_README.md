# Ghidra Export Scripts for psxrecomp

These scripts export function names and labels from Ghidra to CSV format compatible with psxrecomp's `--funcname` flag.

## CSV Format
```
address,name
80021fb8,Init_CharSelectScreen
8001a88c,Game_Main
80030000,ResetVector
```

Address can be with or without `0x` prefix.

---

## Java Script: `ExportFunctionNames.java`

### Install in Ghidra:
1. Open **Window → Script Manager**
2. Click **+** (New Script)
3. Select **Java** as language
4. Paste the contents of `ExportFunctionNames.java`
5. Save as `ExportFunctionNames.java`
6. Click **Run** (green play button) or right-click → **Run**

### Command Line (Headless):
```bash
# Linux/macOS
analyzeHeadless /path/to/project project_name -import /path/to/SCUS_942.36 -scriptPath /path/to/scripts -postScript ExportFunctionNames.java output.csv

# Windows
analyzeHeadless.bat C:\projects\ghidra myproj -import C:\games\SCUS_942.36 -scriptPath C:\scripts -postScript ExportFunctionNames.java output.csv
```

---

## Jython Script: `ExportFunctionNames.py`

### Install in Ghidra:
1. Open **Window → Script Manager**
2. Click **+** (New Script)
3. Select **Python** as language
4. Paste the contents of `ExportFunctionNames.py`
5. Save as `ExportFunctionNames.py`
6. Click **Run**

### Command Line (Headless):
```bash
analyzeHeadless /path/to/project project_name -import /path/to/SCUS_942.36 -scriptPath /path/to/scripts -postScript ExportFunctionNames.py output.csv
```

---

## Using with psxrecomp

```bash
# With CSV file
psxrecomp-game SCUS_942.36 --seeds seeds/ghidra_funcs.txt --out-dir generated --strict --funcname output.csv

# Or in game.toml
[recompiler]
funcname = "output.csv"
```

---

## What Gets Exported

| Symbol Type | Included? | Notes |
|-------------|-----------|-------|
| Named functions (user-renamed) | ✅ | Skips `FUN_XXXX`, `sub_XXXX` |
| Function labels | ✅ | Only non-auto-generated |
| Data labels (LAB_, DAT_, PTR_) | ❌ | Skipped as auto-generated |
| User-created labels | ✅ | If not matching auto-pattern |

---

## Tips

1. **Rename functions in Ghidra first** - The script skips auto-generated names (`FUN_00021fb8`). Only user-renamed functions are exported.

2. **Use meaningful names** - Good names like `Init_CharSelectScreen`, `Game_MainLoop`, `Load_SaveData` make the generated C readable.

3. **Check for conflicts** - If multiple symbols have the same address, the last one wins in the CSV.

4. **Combine with seeds** - Use `--seeds` for function discovery + `--funcname` for naming.

5. **Regenerate after renaming** - If you rename functions in Ghidra, re-run the export script and recompile.