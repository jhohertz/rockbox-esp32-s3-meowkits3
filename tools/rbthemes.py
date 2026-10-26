#!/usr/bin/env python3
"""Theme pack tooling for the MeowKit ESP32-S3 target.

The themes server (https://themes.rockbox.org/) has no entry for meowkits3, so
this tool works through a *proxy target* whose themes are the right shape for
our panel. ibassodx90 is the default: 320x240, touchscreen, and its themes
arrive already rooted at .rockbox/.

Everything fetched is cached under $RBTHEMES_CACHE (default ~/.cache/rbthemes)
and never fetched again unless --refresh is given. The theme server is a
 volunteer-run service: one request per resource, ever, is the point of this
design. Cached state:

  <cache>/<target>.list.ini      the rbutilqt.php listing
  <cache>/archives/<id>.zip      the downloaded theme archive
  <cache>/extracted/<slug>/      the unpacked archive
  <cache>/installed.json         which themes are in the pack

Subcommands:

  list    [--target T] [--refresh]        show what the server offers
  get     <slug|id> [<slug|id> ...]       download + unpack, into the pack
  remove  <slug>                          drop a theme from the pack
  check   [slug]                          audit font refs against the zips
  fonts   [--all]                         build the fonts zip from the tree
  pack    [-o out.zip]                    write rockbox-themes.zip
  show    <slug>                          print a theme's cfg and file list

The audit is against the *union* of the core zip, the fonts zip and the theme
itself: a theme that points at a font living only in rockbox-fonts.zip is not
a failure, because a user who installs both has it. A font in none of the three
is a failure and the theme will splash FONT LOAD ERROR on the device.
"""

import argparse
import html
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime

THEMES_URL = "https://themes.rockbox.org"
LIST_URL = THEMES_URL + "/rbutilqt.php"
BUILD_INFO_URL = "https://download.rockbox.org/build-info"
DEFAULT_TARGET = "ibassodx90"
# Our panel. Themes for other resolutions are still listed, but flagged.
PANEL_W, PANEL_H = 320, 240

CACHE = os.environ.get(
    "RBTHEMES_CACHE", os.path.expanduser("~/.cache/rbthemes")
)
ARCHIVE_DIR = os.path.join(CACHE, "archives")
EXTRACT_DIR = os.path.join(CACHE, "extracted")
INSTALLED = os.path.join(CACHE, "installed.json")

# Where the built artefacts live; used only by the font audit.
DEFAULT_CORE_ZIP = "build-meowkits3/rockbox.zip"
DEFAULT_FONTS_ZIP = "build-meowkits3/rockbox-fonts.zip"

FONT_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.\-]*\.fnt")
TOUCH_RE = re.compile(r"%T\(")
TEXT_FILES = (".wps", ".sbs", ".cfg", ".lds", ".txt")


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def fetch(url, dest):
    """Fetch url to dest. Never called for something already in the cache."""
    req = urllib.request.Request(url, headers={"User-Agent": "rbthemes/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
            shutil.copyfileobj(r, f)
    except urllib.error.URLError as e:
        die("fetch failed: %s (%s)" % (url, e))
    size = os.path.getsize(dest)
    if size == 0:
        os.unlink(dest)
        die("empty response from %s" % url)
    print("fetched %s (%d bytes)" % (url, size))


# --------------------------------------------------------------------------
# listing
# --------------------------------------------------------------------------

def parse_listing(text):
    """rbutilqt.php replies in an INI-ish shape, one section per theme.

    Section name is the theme slug; fields are name, size, author, version,
    about, image, image2, archive (a download.php?themeid=N path) and
    pass_current, which download.php requires.

    Hand-rolled rather than configparser: `about` values span many lines, and
    configparser treats indented continuation lines as errors.
    """
    themes = {}
    sect = None
    kv = {}
    pair = re.compile(r"^\s*([a-z_]+)\s*=\s*(.*)$")

    def flush():
        if not sect or sect in ("error", "status", "dailies", "daily"):
            return
        archive = kv.get("archive", "")
        m = re.search(r"themeid=(\d+)", archive)
        if not m:
            return
        row = dict(kv)
        row["id"] = int(m.group(1))
        row["slug"] = sect
        res = re.search(r"/themes/(\d+)x(\d+)/", row.get("image", ""))
        row["res"] = (int(res.group(1)), int(res.group(2))) if res else None
        themes[row["id"]] = row

    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            flush()
            sect = s[1:-1]
            kv = {}
            continue
        m = pair.match(line)
        if m:
            val = m.group(2)
            if val.startswith('"') and val.endswith('"') and len(val) > 1:
                val = val[1:-1]
            # names come back with XML entities (&amp; etc)
            kv[m.group(1)] = html.unescape(val)
        # anything else is a continuation of a multi-line value; ignored
    flush()
    return themes


def listing(target, refresh=False):
    path = os.path.join(CACHE, target + ".list.ini")
    if refresh or not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        url = "%s?target=%s" % (LIST_URL, target)
        fetch(url, path)
    else:
        print("using cached listing %s (%s, %d bytes)"
              % (os.path.basename(path),
                 datetime.fromtimestamp(os.path.getmtime(path)).strftime("%Y-%m-%d"),
                 os.path.getsize(path)))
    text = open(path, encoding="utf-8", errors="replace").read()
    if re.search(r"code=1", text[:200]):
        die("server has no themes for target '%s'; pick a proxy target" % target)
    return parse_listing(text)


def pass_token(themes):
    """pass_current is the same value for every theme in a listing."""
    for row in themes.values():
        tok = row.get("pass_current")
        if tok:
            return tok
    die("listing has no pass_current token; re-fetch with --refresh")


# --------------------------------------------------------------------------
# installed state
# --------------------------------------------------------------------------

def load_installed():
    if not os.path.exists(INSTALLED):
        return {}
    return json.load(open(INSTALLED, encoding="utf-8"))


def save_installed(state):
    os.makedirs(CACHE, exist_ok=True)
    with open(INSTALLED, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1, sort_keys=True)


# --------------------------------------------------------------------------
# archives
# --------------------------------------------------------------------------

def resolve(themes, want):
    if want.isdigit():
        row = themes.get(int(want))
        if row:
            return row
        die("no theme with id %s" % want)
    low = want.lower()
    for row in themes.values():
        if row["slug"].lower() == low or row.get("name", "").lower() == low:
            return row
    die("no theme named '%s' in this listing" % want)


def get_theme(target, themes, row):
    """Download once, unpack once. Both are cached and reused forever."""
    tok = pass_token(themes)
    zip_path = os.path.join(ARCHIVE_DIR, "%d.zip" % row["id"])
    os.makedirs(ARCHIVE_DIR, exist_ok=True)
    if not os.path.exists(zip_path):
        url = ("%s/download.php?themeid=%d&pass=%s" % (THEMES_URL, row["id"], tok))
        fetch(url, zip_path)
    else:
        print("using cached archive %s (%d bytes)"
              % (os.path.basename(zip_path), os.path.getsize(zip_path)))

    out = os.path.join(EXTRACT_DIR, row["slug"])
    if os.path.isdir(out):
        shutil.rmtree(out)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(out)
    return out


def theme_files(root):
    """Relative paths, .rockbox-stripped, as they will appear in the pack."""
    base = os.path.join(root, ".rockbox")
    if not os.path.isdir(base):
        base = root  # some archives are already stripped
    out = []
    for dirpath, _, names in os.walk(base):
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, base)
            out.append((rel, full))
    return sorted(out)


def theme_texts(root):
    for rel, full in theme_files(root):
        if rel.lower().endswith(TEXT_FILES):
            yield rel, open(full, encoding="utf-8", errors="replace").read()


# --------------------------------------------------------------------------
# font audit
# --------------------------------------------------------------------------

def zip_font_names(path):
    """Font file names carried by a zip, ignoring the directory they live in."""
    if not path or not os.path.exists(path):
        return set()
    names = set()
    with zipfile.ZipFile(path) as z:
        for n in z.namelist():
            if n.lower().endswith(".fnt"):
                names.add(os.path.basename(n).lower())
    return names


def audit(target, themes, slug, core_zip, fonts_zip):
    state = load_installed()
    roots = ({slug: os.path.join(EXTRACT_DIR, slug)} if slug
             else {k: os.path.join(EXTRACT_DIR, k) for k in state})
    if not roots:
        die("nothing installed; run 'get' first")

    core = zip_font_names(core_zip)
    fonts = zip_font_names(fonts_zip)
    avail = core | fonts
    print("available fonts: %d in core, %d in fonts zip" % (len(core), len(fonts)))

    problems = 0
    for name, root in sorted(roots.items()):
        if not os.path.isdir(root):
            print("%-22s MISSING FROM CACHE" % name)
            problems += 1
            continue
        own = {os.path.basename(n).lower()
               for n, _ in theme_files(root) if n.lower().endswith('.fnt')}
        refs = set()
        regions = 0
        cfgs = []
        for rel, text in theme_texts(root):
            refs |= set(FONT_RE.findall(text))
            regions += len(TOUCH_RE.findall(text))
            if rel.lower().endswith(".cfg"):
                cfgs.append(rel)
        missing = sorted(r for r in refs
                         if r.lower() not in own and r.lower() not in avail)
        bad_cfg = []
        for rel, text in theme_texts(root):
            if not rel.lower().endswith(".cfg"):
                continue
            for line in text.splitlines():
                m = re.match(r"\s*(wps|sbs|font)\s*:\s*(\S+)", line)
                if not m:
                    continue
                # '-' is the theme's way of saying "this theme sets none";
                # settings_load_config() stores it verbatim and that is fine.
                if m.group(2) == "-":
                    continue
                if not m.group(2).startswith("/.rockbox/"):
                    bad_cfg.append("%s: %s" % (rel, line.strip()))
        row = "%-22s refs:%-3d own:%-2d regions:%-3d cfg:%d" % (
            name, len(refs), len(own), regions, len(cfgs))
        if missing:
            row += "  MISSING " + ", ".join(missing[:3])
            problems += 1
        if bad_cfg:
            row += "  NON-ABSOLUTE CFG " + bad_cfg[0][:40]
            problems += 1
        print(row)
    print("audit: %d theme(s), %d problem(s)" % (len(roots), problems))
    return problems


# --------------------------------------------------------------------------
# pack
# --------------------------------------------------------------------------

def build_pack(out_path, slugs):
    state = load_installed()
    chosen = slugs or sorted(state)
    if not chosen:
        die("nothing installed; run 'get' first")

    seen = {}
    written = 0
    clash = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as out:
        for slug in chosen:
            root = os.path.join(EXTRACT_DIR, slug)
            if not os.path.isdir(root):
                die("%s is not unpacked in the cache" % slug)
            for rel, full in theme_files(root):
                key = rel.lower()
                digest = hashlib.md5(open(full, "rb").read()).hexdigest()
                if key in seen:
                    if seen[key][1] == digest:
                        continue  # identical file, already in the pack
                    clash += 1
                    print("clash: %s differs between %s and %s; kept %s"
                          % (rel, slug, seen[key][0], seen[key][0]))
                    continue
                seen[key] = (slug, digest)
                out.write(full, ".rockbox/" + rel)
                written += 1
    print("themes in pack: " + ", ".join(state.get(s, {}).get("name", s) for s in chosen))
    print("wrote %s (%d entries, %d bytes%s)"
          % (out_path, written, os.path.getsize(out_path),
             ", %d clash(es)" % clash if clash else ""))
    if clash:
        print("Clashes mean two themes want the same path with different bytes.\n"
              "Ship them as separate packs, or drop one.")


# --------------------------------------------------------------------------
# fonts zip
# --------------------------------------------------------------------------

def build_fonts_zip(repo, out_path):
    """Build every font in the tree. Local only - the server is not involved.

    buildzip.pl -f 1 is the same thing 'make fontzip' runs, called directly
    because that goal drags in rockbox-info.txt and therefore a relink.
    """
    build = os.path.join(repo, "build-meowkits3")
    if not os.path.isdir(build):
        die("no build-meowkits3 directory; configure the target first")
    cmd = ["perl", os.path.join(repo, "tools/buildzip.pl"),
           "-m", "meowkits3", "-i", "126", "-o", out_path,
           "-r", repo, "--rbdir=/.rockbox", "-f", "1",
           "-DMEOWKIT_ESP32S3", "rockbox.esp32s3"]
    print("running: " + " ".join(cmd))
    if subprocess.call(cmd, cwd=build) != 0:
        die("buildzip.pl failed")


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_list(args):
    themes = listing(args.target, args.refresh)
    rows = sorted(themes.values(), key=lambda r: (r.get("name") or "").lower())
    matched = 0
    for row in rows:
        res = row.get("res")
        mark = "=" if res == (PANEL_W, PANEL_H) else " "
        if res == (PANEL_W, PANEL_H):
            matched += 1
        print("%s %-6d %-28s %-16s %s %s"
              % (mark, row["id"], (row.get("name") or row["slug"])[:28],
                 (row.get("author") or "")[:16],
                 "%dx%d" % res if res else "?",
                 row.get("version", "")[:10]))
    print("%d themes, %d at %dx%d" % (len(rows), matched, PANEL_W, PANEL_H))
    print("resolution column comes from the listing image path; themes for\n"
          "other panels still load, they just lay out wrong.")


def cmd_get(args):
    themes = listing(args.target, args.refresh)
    state = load_installed()
    for want in args.themes:
        row = resolve(themes, want)
        root = get_theme(args.target, themes, row)
        state[row["slug"]] = {
            "id": row["id"],
            "name": row.get("name", row["slug"]),
            "author": row.get("author", ""),
            "version": row.get("version", ""),
            "res": list(row.get("res") or []),
            "files": len(theme_files(root)),
        }
        print("%s: %d files unpacked" % (row["slug"], len(theme_files(root))))
    save_installed(state)


def cmd_remove(args):
    state = load_installed()
    if args.slug not in state:
        die("%s is not in the pack" % args.slug)
    del state[args.slug]
    save_installed(state)
    print("removed %s from the pack (cache kept)" % args.slug)


def cmd_check(args):
    themes = listing(args.target, False)
    sys.exit(1 if audit(args.target, themes, args.slug,
                         args.core_zip, args.fonts_zip) else 0)


def cmd_fonts(args):
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    build_fonts_zip(repo, args.out)


def cmd_pack(args):
    build_pack(args.out, args.themes)


def cmd_show(args):
    root = os.path.join(EXTRACT_DIR, args.slug)
    if not os.path.isdir(root):
        die("%s is not unpacked in the cache" % args.slug)
    files = theme_files(root)
    print("%s: %d files" % (args.slug, len(files)))
    for rel, _ in files:
        print("   " + rel)
    for rel, text in theme_texts(root):
        if rel.lower().endswith(".cfg"):
            print("--- %s ---" % rel)
            print(text)


def main():
    global CACHE, ARCHIVE_DIR, EXTRACT_DIR, INSTALLED
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cache", help="cache directory (default %s)" % CACHE)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--target", default=DEFAULT_TARGET,
                        help="proxy target (default %s)" % DEFAULT_TARGET)
        sp.add_argument("--refresh", action="store_true",
                        help="re-fetch the listing (the only way to hit the server again)")

    sp = sub.add_parser("list"); common(sp); sp.set_defaults(func=cmd_list)
    sp = sub.add_parser("get"); common(sp)
    sp.add_argument("themes", nargs="+", help="slug or themeid")
    sp.set_defaults(func=cmd_get)
    sp = sub.add_parser("check"); common(sp)
    sp.add_argument("slug", nargs="?", help="audit one theme instead of the pack")
    sp.add_argument("--core-zip", default=DEFAULT_CORE_ZIP)
    sp.add_argument("--fonts-zip", default=DEFAULT_FONTS_ZIP)
    sp.set_defaults(func=cmd_check)
    sp = sub.add_parser("remove")
    sp.add_argument("slug"); sp.set_defaults(func=cmd_remove)
    sp = sub.add_parser("fonts")
    sp.add_argument("-o", "--out", default="rockbox-fonts.zip")
    sp.set_defaults(func=cmd_fonts)
    sp = sub.add_parser("pack")
    sp.add_argument("themes", nargs="*")
    sp.add_argument("-o", "--out", default="build-meowkits3/rockbox-themes.zip")
    sp.set_defaults(func=cmd_pack)
    sp = sub.add_parser("show")
    sp.add_argument("slug"); sp.set_defaults(func=cmd_show)

    args = p.parse_args()
    if args.cache:
        CACHE = os.path.abspath(args.cache)
        ARCHIVE_DIR = os.path.join(CACHE, "archives")
        EXTRACT_DIR = os.path.join(CACHE, "extracted")
        INSTALLED = os.path.join(CACHE, "installed.json")
    args.func(args)


if __name__ == "__main__":
    main()
