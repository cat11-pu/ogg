#!/usr/bin/env python3
"""Run every corpus case under the sanitizer driver and compare with
the independent Python reference implementation.

Outputs:
  out/run_results.jsonl   one record per case
  out/summary.json        aggregate counters
Any non-zero sanitizer exit (crash/OOB/UBSan/leak) aborts loudly.
"""

import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ogg_model import reference_decode  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)


def parse_driver(text):
    pages, packets, diag, done = [], [], [], None
    for line in text.splitlines():
        if line.startswith("PAGE "):
            kv = dict(tok.split("=", 1) for tok in line.split()[1:])
            pages.append(kv)
        elif line.startswith("PACKET "):
            kv = dict(tok.split("=", 1) for tok in line.split()[1:])
            packets.append(kv)
        elif line.startswith("DIAG "):
            parts = line.split()
            kv = {}
            detail = []
            for tok in parts[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    kv[k] = v
                else:
                    detail.append(tok)
            kv["detail"] = " ".join(detail)
            diag.append(kv)
        elif line.startswith("DONE "):
            done = dict(tok.split("=", 1) for tok in line.split()[1:])
    return pages, packets, diag, done


def compare_valid(path, blob, pages, packets, diag):
    """Diff libogg output against the RFC reference for a valid stream."""
    ref_pages, ref_packets, ref_diag = reference_decode(blob)
    errors = []

    if len(pages) != len(ref_pages):
        errors.append(f"page count {len(pages)} != ref {len(ref_pages)}")
    else:
        for i, (got, want) in enumerate(zip(pages, ref_pages)):
            if int(got["offset"]) != want.offset:
                errors.append(f"page {i} offset {got['offset']} != {want.offset}")
            if int(got["pageno"]) != want.pageno:
                errors.append(f"page {i} pageno mismatch")
            if int(got["body_len"]) != want.body_len:
                errors.append(f"page {i} body_len mismatch")

    if len(packets) != len(ref_packets):
        errors.append(f"packet count {len(packets)} != ref {len(ref_packets)}")
    else:
        for i, (got, want) in enumerate(zip(packets, ref_packets)):
            if int(got["bytes"]) != want["bytes"]:
                errors.append(f"packet {i} bytes {got['bytes']} != {want['bytes']}")
            if int(got["b_o_s"]) != want["b_o_s"]:
                errors.append(f"packet {i} b_o_s mismatch")
            if int(got["e_o_s"]) != want["e_o_s"]:
                errors.append(f"packet {i} e_o_s mismatch")

    # Reference must not diagnose structural faults on valid corpus.
    bad_ref = [d for d in ref_diag if d["reason"]]
    if bad_ref:
        errors.append(f"reference flagged valid stream: {bad_ref[:2]}")
    return errors, ref_pages, ref_packets


def expected_failure(label, diag, pages):
    """Each malformed case must produce an offset+reason diagnostic."""
    reasons = [d.get("reason") for d in diag if d.get("reason")]
    if not reasons:
        return False, "no failure diagnostic emitted"
    has_offset = all("offset" in d for d in diag if d.get("reason"))
    if not has_offset:
        return False, "diagnostic missing offset"
    family_map = {
        "truncation": {"truncated_header", "page_length_mismatch",
                       "checksum_mismatch"},
        "page_length_mismatch": {"page_length_mismatch",
                                 "truncated_header", "checksum_mismatch"},
        "bad_checksum": {"checksum_mismatch", "page_length_mismatch",
                         "truncated_header"},
        "illegal_version": {"illegal_version", "checksum_mismatch"},
        "cyclic_page_chain": set(),  # pages are valid; chain is tolerated
    }
    allowed = family_map[label]
    if label == "cyclic_page_chain":
        if "cyclic_page_chain" not in reasons:
            return False, f"chain fault not detected; reasons={reasons}"
    elif allowed and not any(r in allowed for r in reasons):
        return False, f"reasons {reasons} not in {sorted(allowed)}"
    return True, ",".join(sorted(set(reasons)))


def main():
    driver = os.path.join(HERE, "fuzz_parse")
    manifest = os.path.join(HERE, "cases", "manifest.tsv")
    records = []
    crashes = []
    mismatches = []
    with open(manifest) as fh:
        entries = [line.strip().split("\t") for line in fh]

    def run_one(entry):
        name, label, size = entry
        directory = "valid" if label == "valid" else "malformed"
        path = os.path.join(HERE, "cases", directory, name)
        with open(path, "rb") as fh:
            blob = fh.read()
        proc = subprocess.run([driver, path], capture_output=True, text=True)
        if proc.returncode != 0:
            return {"crash": {"case": name, "rc": proc.returncode,
                              "stderr": proc.stderr[-2000:]}}
        pages, packets, diag, done = parse_driver(proc.stdout)
        rec = {"case": name, "label": label, "bytes": int(size),
               "pages": len(pages), "packets": len(packets),
               "reasons": sorted({d["reason"] for d in diag
                                  if d.get("reason")}),
               "status": done.get("status") if done else "no_done"}
        if label == "valid":
            errs, _, _ = compare_valid(path, blob, pages, packets, diag)
            rec["reference_match"] = not errs
            if errs:
                rec["errors"] = errs[:5]
                return {"rec": rec,
                        "mismatch": {"case": name, "errors": errs[:5]}}
        else:
            ok, detail = expected_failure(label, diag, pages)
            rec["failure_classified"] = ok
            rec["failure_detail"] = detail
            if not ok:
                return {"rec": rec, "mismatch": {"case": name,
                                                  "errors": [detail]}}
        return {"rec": rec}

    crashes, mismatches = [], []
    records = []
    done_count = 0
    with ThreadPoolExecutor(max_workers=min(12, os.cpu_count() or 4)) as ex:
        futures = [ex.submit(run_one, e) for e in entries]
        for fut in as_completed(futures):
            result = fut.result()
            done_count += 1
            if "crash" in result:
                crashes.append(result["crash"])
            if "rec" in result:
                records.append(result["rec"])
            if "mismatch" in result:
                mismatches.append(result["mismatch"])
            if done_count % 250 == 0:
                print(f"  ...{done_count}/{len(entries)}", flush=True)

    records.sort(key=lambda r: r["case"])

    with open(os.path.join(OUT, "run_results.jsonl"), "w") as fh:
        for rec in records:
            fh.write(json.dumps(rec) + "\n")

    summary = {
        "total": len(entries),
        "crashes": crashes,
        "mismatches": mismatches,
        "by_label": {},
        "valid_reference_matches": sum(
            1 for r in records if r["label"] == "valid"
            and r.get("reference_match")),
        "malformed_classified": sum(
            1 for r in records if r["label"] != "valid"
            and r.get("failure_classified")),
    }
    for rec in records:
        summary["by_label"].setdefault(rec["label"], 0)
        summary["by_label"][rec["label"]] += 1
    with open(os.path.join(OUT, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)

    print(json.dumps({k: v for k, v in summary.items()
                      if k not in ("crashes", "mismatches")}, indent=2))
    if crashes:
        print(f"FATAL: {len(crashes)} sanitizer failures", file=sys.stderr)
        sys.exit(1)
    if mismatches:
        print(f"FATAL: {len(mismatches)} reference/classification mismatches",
              file=sys.stderr)
        sys.exit(2)
    print("ALL CASES CLEAN")


if __name__ == "__main__":
    main()
