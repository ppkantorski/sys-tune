##################################################################################
# File: sdout.py — SDOut Package Builder
#
# Description:
#   This script automates the creation of `sdout.zip`, a complete deployment
#   package for sys-tune. It organizes and assembles sys-tune components into
#   a proper SD card structure ready for use.
#
#   Layout assembled (relative to SD card root):
#     switch/.overlays/sys-tune-overlay.ovl
#     atmosphere/contents/4200000000000000/exefs.nsp
#     atmosphere/contents/4200000000000000/toolbox.json
#     atmosphere/contents/4200000000000000/flags/          (empty)
#
# Source layout expected (relative to this script):
#     overlay/sys-tune-overlay.ovl
#     sys-tune/sys-tune.nsp
#     sys-tune/toolbox.json
#
# Licensed under GPLv2
##################################################################################

import os
import shutil
import zipfile
from pathlib import Path


TITLE_ID = "4200000000000000"


def create_zip_without_metadata(source_dir, output_zip):
    """Create a zip file excluding metadata files, preserving empty directories."""
    print(f"Creating {output_zip}...")
    with zipfile.ZipFile(output_zip, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for root, dirs, files in os.walk(source_dir):
            # Write an explicit entry for empty directories
            if not files and not dirs:
                dir_arcname = os.path.relpath(root, source_dir) + '/'
                zipf.mkdir(dir_arcname)
                continue

            for file in files:
                # Skip macOS metadata files
                if file.startswith('._') or file == '.DS_Store':
                    continue

                file_path = os.path.join(root, file)
                arcname = os.path.relpath(file_path, source_dir)
                zipf.write(file_path, arcname)
    print(f"Created {output_zip}")


def main():
    script_dir = Path.cwd()
    sdout_dir  = script_dir / "sdout"
    sdout_zip  = script_dir / "sdout.zip"

    # ── Source paths ──────────────────────────────────────────────────────────
    ovl_source     = script_dir / "overlay" / "sys-tune-overlay.ovl"
    nsp_source     = script_dir / "sys-tune" / "sys-tune.nsp"
    toolbox_source = script_dir / "sys-tune" / "toolbox.json"

    # Validate sources before doing anything
    missing = [p for p in (ovl_source, nsp_source, toolbox_source) if not p.exists()]
    if missing:
        for p in missing:
            print(f"✗ Missing source file: {p}")
        raise SystemExit(1)

    # ── Clean up previous build ───────────────────────────────────────────────
    print("Cleaning up previous build...")
    if sdout_dir.exists():
        shutil.rmtree(sdout_dir)
        print("  Deleted existing sdout/")
    if sdout_zip.exists():
        sdout_zip.unlink()
        print("  Deleted existing sdout.zip")

    # ── Create folder structure ───────────────────────────────────────────────
    print("Creating folder structure...")
    contents_dir = sdout_dir / "atmosphere" / "contents" / TITLE_ID
    flags_dir    = contents_dir / "flags"
    overlays_dir = sdout_dir / "switch" / ".overlays"

    for folder in (contents_dir, flags_dir, overlays_dir):
        folder.mkdir(parents=True, exist_ok=True)
        print(f"  Created {folder.relative_to(sdout_dir)}")

    # ── Copy files ────────────────────────────────────────────────────────────
    print("Copying files...")

    shutil.copy2(ovl_source, overlays_dir / "sys-tune-overlay.ovl")
    print(f"  Copied sys-tune-overlay.ovl → switch/.overlays/")

    shutil.copy2(nsp_source, contents_dir / "exefs.nsp")
    print(f"  Copied sys-tune.nsp → atmosphere/contents/{TITLE_ID}/exefs.nsp")

    shutil.copy2(toolbox_source, contents_dir / "toolbox.json")
    print(f"  Copied toolbox.json → atmosphere/contents/{TITLE_ID}/")

    # flags/ is intentionally left empty
    print(f"  Created empty flags/ directory")

    # ── Package ───────────────────────────────────────────────────────────────
    create_zip_without_metadata(sdout_dir, sdout_zip)

    print(f"\n✓ Successfully created sdout.zip!")
    print(f"  Location: {sdout_zip}")


if __name__ == "__main__":
    main()