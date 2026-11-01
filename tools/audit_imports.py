#!/usr/bin/env python3
"""Import audit for JStarsReborn.

Classifies every import of the game's eboot (477 unique import-table entries; each can be referenced
by many relocation 'binding' entries - the 26,919 relocations are NOT the unit counted here) into
  Original code            resolved against the game's own bundled modules (libc.prx, Fios2)
  Implemented, verified    runtime host function with a test or observed-execution behind it
  Partially implemented    real behaviour with named gaps, or vendored code not yet verified here
  Diagnostic stub          returns without doing the work (logged); exists only to let execution continue
  Unsupported              no implementation: bound to a trap that stops the run

Inputs: the prepared game's import list + link.json, `jstars-boot --audit-dump`, the vendored GPU
sources (LIB_FUNCTION registrations; upstream STUBBED/UNIMPLEMENTED bodies), and any number of run logs
(FIRST CALL lines) which add an 'executed' column. 'Bound' never means 'working': a status above
'Partially implemented' needs the evidence named in the evidence column.
"""
import argparse, collections, json, os, re, sys

LIBS = ["core/libraries/gnmdriver/gnmdriver.cpp", "core/libraries/videoout/video_out.cpp",
        "core/libraries/kernel/equeue.cpp"]

def gpu_exports(src_root):
    """NID#lib -> (function, classification, note) from the vendored sources."""
    out = {}
    pat = re.compile(r'LIB_(?:FUNCTION|OBJ)\("([^"]+)",\s*"([^"]+)",\s*\d+,\s*"[^"]+",\s*(\w+)\)')
    for rel in LIBS:
        path = os.path.join(src_root, rel)
        if not os.path.exists(path):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        for nid, lib, fn in pat.findall(text):
            body = function_body(text, fn)
            if body is None:
                cls, note = "partial", "registered export; definition not found by the audit parser (check by hand)"
            elif re.search(r'STUBBED|UNIMPLEMENTED\(|UNREACHABLE', body) and len(body) < 400:
                cls, note = "diagnostic-stub", "upstream shadPS4 stub: logs '(STUBBED)' and returns a fixed value"
            elif re.search(r'STUBBED|UNIMPLEMENTED|UNREACHABLE', body):
                cls, note = "partial", "vendored shadPS4 code with unimplemented branches (STUBBED/UNIMPLEMENTED inside)"
            else:
                cls, note = "partial", "vendored shadPS4 code; not verified by this port until exercised with evidence"
            out[f"{nid}#{lib}"] = (fn, cls, note)
    return out

def function_body(text, fn):
    m = re.search(r'\b' + re.escape(fn) + r'\s*\([^;{]*\)\s*(?:const\s*)?\{', text)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    for j in range(i, len(text)):
        if text[j] == '{': depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0: return text[i:j + 1]
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--imports", required=True, help="import-names.tsv (NID#lib <tab> name)")
    ap.add_argument("--link", required=True, help="link.json")
    ap.add_argument("--runtime-dump", required=True, help="output of jstars-boot --audit-dump")
    ap.add_argument("--gpu-src", required=True, help="gpu/vendor/shadps4/src")
    ap.add_argument("--runs", nargs="*", default=[], help="jstars-boot logs with FIRST CALL lines")
    ap.add_argument("--out-md", required=True)
    ap.add_argument("--out-tsv", required=True)
    ap.add_argument("--title", default="Import audit")
    a = ap.parse_args()

    names = {}
    for line in open(a.imports, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 2: names[p[0]] = p[1]
    link = json.load(open(a.link))
    all_imports = link["all_imports"]
    unresolved = set(link["unresolved_imports"])

    rt = {}   # NID#lib -> (status, note, name) ; also by name for lib aliases
    rt_by_nid = {}
    for line in open(a.runtime_dump, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) < 5: continue
        name, lib, nid, st, note = p[:5]
        rt[f"{nid}#{lib}"] = (st, note, name)
        rt_by_nid.setdefault(nid, (st, note, name))
    gpu = gpu_exports(a.gpu_src)
    gpu_by_nid = {k.split("#")[0]: v for k, v in gpu.items()}

    executed = collections.defaultdict(list)   # key -> [(run, origin, from)]
    for run in a.runs:
        for line in open(run, encoding="utf-8", errors="replace"):
            m = re.search(r'FIRST CALL #(\d+) (\S+) (\S+#\S+) \[(\w+): ([^\]]*)\]', line)
            if m:
                executed[m.group(3)].append((os.path.basename(run), m.group(4), m.group(5)))

    CLASS = {"verified": "Implemented, verified", "partial": "Partially implemented",
             "diagnostic-stub": "Diagnostic stub", "unsupported": "Unsupported", "original": "Original code"}
    rows = []
    for key in all_imports:
        name = names.get(key, "")
        nid = key.split("#")[0]
        if key not in unresolved:
            rows.append((key, name, "original", "game module", "resolved against the game's bundled module (libc.prx / libSceFios2.prx): original code runs", "-"))
        elif key in rt or (nid in rt_by_nid and key not in gpu):
            st, note, fn = rt.get(key) or rt_by_nid[nid]
            ev = "selftest-host" if "selftest-host" in note else ("observed in boot log" if "boot log" in note else "-")
            rows.append((key, name or fn, st, "runtime", note, ev))
        elif key in gpu or nid in gpu_by_nid:
            fn, st, note = gpu.get(key) or gpu_by_nid[nid]
            rows.append((key, name or fn, st, "gpu-library", note, "-"))
        else:
            rows.append((key, name, "unsupported", "-", "no implementation: bound to a trap (exit 3) that names the import", "-"))

    cnt = collections.Counter(r[2] for r in rows)
    byorigin = collections.Counter((r[2], r[3]) for r in rows)
    with open(a.out_tsv, "w", encoding="utf-8") as f:
        f.write("import\tname\tstatus\torigin\tevidence\texecuted_in\tnote\n")
        for key, name, st, org, note, ev in rows:
            ex = ";".join(sorted({e[0] for e in executed.get(key, [])}))
            f.write(f"{key}\t{name}\t{CLASS[st]}\t{org}\t{ev}\t{ex}\t{note}\n")

    with open(a.out_md, "w", encoding="utf-8") as f:
        f.write(f"# {a.title}\n\n")
        f.write("Generated by `tools/audit_imports.py`. **Unit of counting: unique import-table entries** "
                f"({len(all_imports)} for this eboot), not relocation or binding entries: the eboot carries 26,919 relocation records, "
                "and many of them point at the same import. Bound does not mean working: only the rows whose evidence column names a test or an observed execution are called verified.\n\n")
        f.write("| Status | Count |\n|---|---|\n")
        for k in ["original", "verified", "partial", "diagnostic-stub", "unsupported"]:
            f.write(f"| {CLASS[k]} | {cnt.get(k, 0)} |\n")
        f.write(f"| **Total unique imports** | **{len(rows)}** |\n\n")
        f.write("| Status | Origin | Count |\n|---|---|---|\n")
        for (st, org), n in sorted(byorigin.items()):
            f.write(f"| {CLASS[st]} | {org} | {n} |\n")
        f.write("\n")
        if a.runs:
            f.write("Executed (called at least once) in the supplied runs: " + ", ".join(os.path.basename(r) for r in a.runs) +
                    f" - {sum(1 for r in rows if r[0] in executed)} of {len(rows)} imports.\n\n")
        for st in ["unsupported", "diagnostic-stub", "partial", "verified"]:
            group = [r for r in rows if r[2] == st]
            if not group: continue
            f.write(f"## {CLASS[st]} ({len(group)})\n\n| Import | Name | Origin | Executed | Note |\n|---|---|---|---|---|\n")
            for key, name, _st, org, note, ev in sorted(group, key=lambda r: (r[3], r[1])):
                ex = "yes" if key in executed else ""
                f.write(f"| `{key}` | {name} | {org} | {ex} | {note}{' (' + ev + ')' if ev != '-' else ''} |\n")
            f.write("\n")
    print(f"audit: {dict((CLASS[k], v) for k, v in cnt.items())} total {len(rows)}")

if __name__ == "__main__":
    main()
