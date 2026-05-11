#!/usr/bin/env python3
# Interactive WE-2 model bundle picker. Fetches the SenseCraft catalog,
# prompts for up to 5 picks, smart-arranges them into the per-slot capacity,
# downloads chosen .tflite files, writes firmware/grove/models/selected.txt
# (tab-separated: slot_id, flash_addr, local_path, alias, classes_pipe).
#
# Non-interactive (no tty, GROVE_NONINTERACTIVE=1, or --defaults) writes
# the canonical 5-model default bundle.

import json
import os
import re
import shutil
import sys
import urllib.request
from pathlib import Path

REPO_ROOT      = Path(__file__).resolve().parent.parent
CATALOG_URL    = "https://files.seeedstudio.com/sscma/sscma-model-we2.json"
CATALOG_CACHE  = REPO_ROOT / "firmware" / "grove" / "sscma-model-we2.json"
MODELS_DIR     = REPO_ROOT / "firmware" / "grove" / "models"
MANIFEST_PATH  = MODELS_DIR / "selected.txt"

# Slot layout: (flash_address, max bytes before the next slot or partition
# end at 0xE00000). 10 MB total. Slot 4 is tight (1.48 MB).
SLOT_LAYOUT = [
    ("0x400000", 0x200000),  # 2.00 MB
    ("0x600000", 0x200000),  # 2.00 MB
    ("0x800000", 0x200000),  # 2.00 MB
    ("0xA00000", 0x17B000),  # 1.48 MB
    ("0xB7B000", 0x285000),  # 2.52 MB
]
SLOT_ADDRESSES = [a for a, _ in SLOT_LAYOUT]
SLOT_CAPACITIES = [c for _, c in SLOT_LAYOUT]
MAX_SLOTS      = len(SLOT_ADDRESSES)

# Canonical default bundle: (alias, classes-pipe, catalog-URL substring).
DEFAULT_BUNDLE = [
    ("person",  "person",                "swift_yolo_nano_person_192_int8_vela.tflite"),
    ("face",    "face",                  "swift_yolo_1xb16_300e_coco_300_int8_sha1_2287b951101007d4cd1d09c3da68e53e6f23a071_vela.tflite"),
    ("gesture", "paper|rock|scissors",   "swift_yolo_1xb16_300e_coco_sha1_8d25b2b0be2a0ea38d3fe0aca5ce3891f7aa67c5_vela.tflite"),
    ("pet",     "cat|dog",               "animal_detection_int8_vela.tflite"),
    ("apple",   "apple",                 "apple_detection_int8_vela.tflite"),
]


def fetch_catalog():
    CATALOG_CACHE.parent.mkdir(parents=True, exist_ok=True)
    if not CATALOG_CACHE.exists():
        sys.stdout.write("Fetching catalog from %s...\n" % CATALOG_URL)
        with urllib.request.urlopen(CATALOG_URL, timeout=30) as r:
            CATALOG_CACHE.write_bytes(r.read())
    return json.loads(CATALOG_CACHE.read_text())


def derive_alias(name):
    # lowercase, runs of non-alnum -> "_", trim leading/trailing "_".
    a = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return a or "model"


def fmt_classes(classes, limit=3):
    if not classes:
        return "?"
    if len(classes) <= limit:
        return ",".join(classes)
    return "%s+%d" % (",".join(classes[:limit]), len(classes) - limit)


def print_table(models):
    sys.stdout.write("\n  #   Name                                  Algorithm                       Classes               Inf(ms)\n")
    sys.stdout.write("  --  ------------------------------------  ------------------------------  --------------------  -------\n")
    for i, m in enumerate(models, 1):
        name = (m.get("name") or "")[:36]
        algo = (m.get("algorithm") or "")[:30]
        cls  = fmt_classes(m.get("classes") or [])[:20]
        inf  = (m.get("metrics") or {}).get("Inference(ms)", {}).get("we2", "")
        inf_str = ("%.0f" % inf) if isinstance(inf, (int, float)) else ""
        sys.stdout.write("  %2d  %-36s  %-30s  %-20s  %s\n" % (i, name, algo, cls, inf_str))
    sys.stdout.write("\n")


def download(url, dest):
    sys.stdout.write("  downloading %s ... " % dest.name)
    sys.stdout.flush()
    with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)
    sys.stdout.write("%d KB\n" % (dest.stat().st_size // 1024))


def fmt_bytes(n):
    if n >= 1024 * 1024:
        return "%.2f MB" % (n / 1024.0 / 1024.0)
    return "%d KB" % (n // 1024)


def probe_size(url):
    """HEAD first; on 403/404/405 fall back to a Range GET that returns the
    full size via Content-Range. Returns 0 if both paths fail."""
    try:
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=15) as r:
            cl = r.headers.get("Content-Length")
            if cl:
                return int(cl)
    except urllib.error.HTTPError as e:
        if e.code not in (403, 404, 405):
            raise
    except Exception:
        pass

    try:
        req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(req, timeout=15) as r:
            cr = r.headers.get("Content-Range") or ""
            if "/" in cr:
                total = cr.rsplit("/", 1)[1]
                if total.isdigit():
                    return int(total)
    except Exception:
        pass
    return 0


def auto_arrange(picks_with_sizes):
    """Greedy fit: biggest model first into the smallest unused slot that
    fits. Returns [(slot_idx, model, alias, classes, size), ...] sorted by
    slot_idx ascending, or None if any model has no fitting slot."""
    sorted_picks = sorted(
        enumerate(picks_with_sizes), key=lambda x: -x[1][3]
    )
    used = [False] * MAX_SLOTS
    assigned = {}
    for pick_idx, (_model, alias, _classes, size) in sorted_picks:
        best = None
        for s in range(MAX_SLOTS):
            if used[s]:
                continue
            cap = SLOT_CAPACITIES[s]
            if size <= cap and (best is None or cap < SLOT_CAPACITIES[best]):
                best = s
        if best is None:
            sys.stderr.write(
                "\nNo slot can hold model '%s' (%s).\n" % (alias, fmt_bytes(size))
            )
            sys.stderr.write("Slot capacities:\n")
            for i, (addr, cap) in enumerate(SLOT_LAYOUT, 1):
                used_marker = " (taken)" if used[i - 1] else ""
                sys.stderr.write(
                    "  slot %d  %s  %s%s\n" % (i, addr, fmt_bytes(cap), used_marker)
                )
            return None
        used[best] = True
        assigned[pick_idx] = best

    out = []
    for pick_idx in sorted(assigned, key=lambda p: assigned[p]):
        model, alias, classes, size = picks_with_sizes[pick_idx]
        out.append((assigned[pick_idx], model, alias, classes, size))
    return out


def find_model_by_url_substr(catalog_models, substr):
    for m in catalog_models:
        if substr in (m.get("url") or ""):
            return m
    return None


def select_defaults(catalog_models):
    out = []
    for alias, classes, substr in DEFAULT_BUNDLE:
        m = find_model_by_url_substr(catalog_models, substr)
        if m is None:
            sys.stderr.write("warning: default bundle entry '%s' not in catalog; skipping\n" % alias)
            continue
        out.append((m, alias, classes))
    return out


def select_interactive(catalog_models):
    print_table(catalog_models)
    sys.stdout.write("Pick up to %d model numbers separated by spaces (Enter for default bundle: %s):\n> "
                     % (MAX_SLOTS, " ".join(a for a, _, _ in DEFAULT_BUNDLE)))
    sys.stdout.flush()
    raw = sys.stdin.readline()
    if raw is None:
        return select_defaults(catalog_models)
    raw = raw.strip()
    if not raw:
        return select_defaults(catalog_models)

    parts = raw.replace(",", " ").split()
    try:
        nums = [int(p) for p in parts]
    except ValueError:
        sys.stderr.write("Could not parse '%s' as a list of numbers. Falling back to defaults.\n" % raw)
        return select_defaults(catalog_models)

    if not nums:
        return select_defaults(catalog_models)
    if len(nums) > MAX_SLOTS:
        sys.stderr.write("Picked %d models, max is %d. Falling back to defaults.\n" % (len(nums), MAX_SLOTS))
        return select_defaults(catalog_models)
    for n in nums:
        if n < 1 or n > len(catalog_models):
            sys.stderr.write("Index %d out of range 1..%d. Falling back to defaults.\n" % (n, len(catalog_models)))
            return select_defaults(catalog_models)

    out = []
    for n in nums:
        m = catalog_models[n - 1]
        alias = derive_alias(m.get("name") or "model")
        classes = "|".join(m.get("classes") or [])
        out.append((m, alias, classes))
    return out


def write_manifest_arranged(final_picks):
    """final_picks: [(model, alias, classes, slot_idx)] sorted ascending by
    slot_idx. The WE-2 numbers slots 1..N in ascending address order so
    position+1 is the WE-2 id."""
    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    lines = []
    for we2_id, (model, alias, classes, phys_idx) in enumerate(final_picks, 1):
        addr = SLOT_ADDRESSES[phys_idx]
        local = MODELS_DIR / ("%s.tflite" % alias)
        if not local.exists():
            download(model["url"], local)
        rel = local.relative_to(REPO_ROOT)
        lines.append("%d\t%s\t%s\t%s\t%s" % (we2_id, addr, rel, alias, classes))
    MANIFEST_PATH.write_text("\n".join(lines) + "\n")
    return lines


def main(argv):
    use_defaults = "--defaults" in argv or os.environ.get("GROVE_NONINTERACTIVE")

    catalog = fetch_catalog()
    def is_we2(m):
        d = m.get("devices")
        if d == "we2":
            return True
        if isinstance(d, list) and "we2" in d:
            return True
        return False
    models = [m for m in catalog.get("models", []) if is_we2(m)]
    if not models:
        sys.stderr.write("Catalog has no we2 models. Aborting.\n")
        return 1

    if use_defaults or not sys.stdin.isatty():
        picks = select_defaults(models)
    else:
        picks = select_interactive(models)

    if not picks:
        sys.stderr.write("No models selected. Aborting.\n")
        return 1

    # Probe sizes. SenseCraft's CDN 403s on HEAD for some URLs, hence the
    # Range-GET fallback inside probe_size.
    sys.stdout.write("\nProbing model sizes...\n")
    picks_with_sizes = []
    for model, alias, classes in picks:
        size = probe_size(model["url"])
        if size <= 0:
            sys.stderr.write("  %s: size probe failed (CDN returned no length)\n" % alias)
            return 1
        sys.stdout.write("  %-26s  %s\n" % (alias, fmt_bytes(size)))
        picks_with_sizes.append((model, alias, classes, size))

    # Place biggest model in smallest fitting slot. WE-2 ids are then 1..N
    # in ascending address order.
    arranged = auto_arrange(picks_with_sizes)
    if arranged is None:
        return 1

    sys.stdout.write("\nSlot assignment:\n")
    final_picks = []
    for we2_id, (phys_idx, model, alias, classes, size) in enumerate(arranged, 1):
        addr, cap = SLOT_LAYOUT[phys_idx]
        sys.stdout.write("  id %d  %s (cap %s)  : %-22s  %s\n"
                         % (we2_id, addr, fmt_bytes(cap), alias, fmt_bytes(size)))
        final_picks.append((model, alias, classes, phys_idx))

    lines = write_manifest_arranged(final_picks)
    sys.stdout.write("\nWrote %s with %d slot(s):\n" % (MANIFEST_PATH, len(lines)))
    for ln in lines:
        sys.stdout.write("  %s\n" % ln)
    sys.stdout.write("\nNext steps:\n")
    sys.stdout.write("  1. `make flash-grove`         streams firmware + selected models to the WE-2\n")
    sys.stdout.write("  2. `make set-grove-aliases`   pushes the alias + class table to the C3's NVS\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
