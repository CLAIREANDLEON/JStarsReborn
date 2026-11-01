# Local data inventory and future import boundary

## Implemented utility

`tools/asset_audit.py` is a read-only file inventory. It is not an importer, converter, PS4 parser or asset validator. It accepts an existing local directory, computes streaming SHA-256, records size and up to 64 header bytes, and emits standard-format signature hints. An extension or signature never sets `validated_asset` to true. PNG, DDS, KTX2, RIFF/WAVE, Ogg, FLAC, ELF and MZ hints only recognize header patterns; complete structures are not parsed.

Run from the project directory with Python 3.10 or later:

```powershell
python tools/asset_audit.py C:\LocalReferenceData --output C:\AuditReports\inventory.json
python -m unittest discover -s tests -p test_asset_audit.py -v
```

The output directory must already exist and must be outside the input. Files beneath input are never intentionally written. The report is atomically replaced after flushing its temporary file. Keep reports local: header fragments, paths and hashes may identify proprietary or private material.

Exit status is `0` for a complete inventory, `1` for a written but incomplete inventory, and `2` for invalid arguments or a failure to produce the report. Missing/unreadable files, changed files, symbolic links, reparse points, unsupported filesystem objects and exceeded limits become diagnostics, not silent successes. File records and diagnostics are sorted for repeatable comparison.

## Bounds and filesystem behavior

| Option | Default | Meaning |
| --- | --- | --- |
| `--max-entries` | 100000 | Maximum encountered directory entries |
| `--max-file-bytes` | 4294967296 | Maximum single-file size |
| `--max-total-bytes` | 68719476736 | Maximum data bytes read, including reads that later fail verification |
| `--max-depth` | 32 | Maximum descended folder depth below input |

Reads use at most 1 MiB per block. Header retention is limited to 64 bytes. The tool checks file identity/size/timestamps before opening and after reading. On Windows it verifies the opened handle's final path before reading; on Linux it requires `/proc/self/fd`. Other platforms fail closed when opened-path verification is unavailable. Existing symbolic links and Windows reparse points in supplied paths are rejected.

Use a stable, non-adversarial copy of the inputs. This utility is not a forensic snapshot or a security boundary against a process concurrently rewriting data and restoring metadata. Maximum-entry truncation is explicitly incomplete; its retained subset may depend on filesystem enumeration order. It makes no claim that signatures, inaccessible packages, or header hashes reveal game semantics.

## Integration gates still blocked

1. Supply legitimate accessible data and record its permission/provenance scope. Inventory without changing originals.
2. Establish the actual container and resource specifications with bounded fixtures. Do not invent a format or treat a filename as proof.
3. Implement and test each parser independently, including lengths, offsets, decompression bounds, invalid indices and resource budgets.
4. Convert into a documented engine-owned intermediate representation while preserving original timing, coordinate and material semantics established by evidence.
5. Integrate character, stage, animation and audio resources, then compare against reference trials.

No proprietary-format parser, texture streaming, replacement lookup, mod loader, script sandbox, content exporter, save importer or game-data conversion is implemented by this inventory utility. Future overlays must leave originals unchanged, validate dependency/version conflicts and distinguish cosmetic from gameplay content. Online compatibility hashes and a stable mod interface depend on the eventual verified data model.

## Observed timestamp issue and regression fix

On the tested Windows/Python host, `lstat` and `fstat` sometimes returned different `st_ctime_ns` values for a newly written file with identical device ID, file ID, size and modification time. Comparing that deprecated Windows field caused false changed-file diagnostics. Windows identity checks now compare the stable IDs, size and `st_mtime_ns`; POSIX additionally compares `st_ctime_ns`. A synthetic regression covers this distinction, and the growth-during-read test still requires rejection. Python documents the [deprecated Windows ctime semantics](https://docs.python.org/3/library/os.html#os.stat_result.st_ctime).
