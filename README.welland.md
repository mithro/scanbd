# scanbd — welland pixma button-fix apt package

This repo (`main`) packages a **patched build of scanbd 1.5.1** (Debian's
`scanbd 1.5.1-7` sources, re-homed as a `3.0 (native)` package) as a
GPG-signed Debian apt repository for **arm64** and **amd64**, served from
GitHub Pages at <https://mithro.github.io/scanbd/>.

scanbd is licensed **GPL-2+** (upstream © 2008–2017 Wilhelm Meier); this build
keeps that license. Only the packaging (`debian/`, `packaging/`) and one C
patch (below) are added on top of the upstream/Debian sources.

## Why — physical button presses never fire with the pixma backend

On the welland fleet's `rpi5-scanner` host (a Canon **LiDE 400** flatbed, SANE
**pixma** backend, `pixma:04A91912_498A13`), stock scanbd runs and polls the
device but **never fires an action script on a physical button press.**

Root cause, from the SANE backend source
(`backend/pixma/pixma.c`, `control_option()` / `update_button_state()`):

- The pixma backend caches the scanner's button/event words (`button-1`,
  `button-2`, `target`, `original`, resolution, …). Those cached words are
  **only refreshed from the device as a side effect of an *ACTION* on the
  `SANE_TYPE_BUTTON` option `button-update`** — and specifically **only on
  `SANE_ACTION_SET_VALUE`**. A `GET_VALUE` on `button-update` returns
  `SANE_STATUS_INVAL` and refreshes nothing.
- `scanimage -A` reports presses because it **walks and reads every option**
  in one `sane_open`. Reading the low-index button options (which are
  "uncached") makes the backend run its internal `update_button_state()`
  refresh, so a subsequent read of `target` returns the real, just-pressed
  value.
- Stock scanbd's poll loop only issues **`GET_VALUE`**, and only on the handful
  of options that carry a configured `action`/`function`. It therefore never
  drives the pixma refresh: `target`/`button-*` stay at their stale cached
  word and no press is ever observed.

A **config-only workaround was tried and failed**: adding a
`function button-update { … }` as the first function so scanbd reads it before
`target`. Logs confirmed scanbd read `button-update` (option 16) before
`target` (option 20) on every poll — **but presses still did not fire**, because
scanbd *GETs* `button-update` (a no-op that returns `INVAL`) rather than
*SETting* it. The fix has to be in scanbd's C source.

## The fix

`src/scanbd/sane.c` gains a small helper, `sane_refresh_button_state()`, called
once at the **top of every poll pass, before the monitored options are read**.
It finds the active, settable `SANE_TYPE_BUTTON` option named `button-update`
and issues a **`SANE_ACTION_SET_VALUE`** on it — the backend-documented
"Update button state" trigger — so the cached button/event words are refreshed
on the same open handle before scanbd reads them. This mirrors, deliberately
and minimally, the refresh `scanimage -A` gets for free from its full
option-walk.

Backends **without** a `button-update` option are unaffected — the option is
simply not found and nothing is set. No other button option is ever SET, so no
unrelated scanner action (calibrate, etc.) is triggered.

## Install

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://mithro.github.io/scanbd/scanbd.gpg \
  | sudo tee /etc/apt/keyrings/mithro-scanbd.gpg > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/mithro-scanbd.gpg] https://mithro.github.io/scanbd/ ./" \
  | sudo tee /etc/apt/sources.list.d/mithro-scanbd.list
sudo apt update
sudo apt install scanbd
```

The package version is `1.5.1+welland1`. The `+welland<N>` local suffix sorts
**above** Debian's `1.5.1-7` (so this build installs over it) and is cleanly
superseded by any future official upstream release (`1.5.2`, `1.6.x`, …).

## How it's built

`.github/workflows/deb.yml` builds native arm64 + amd64 `.deb`s (in a
`debian:trixie` container) from this repo's `debian/` packaging, then publishes
a signed flat apt repo to GitHub Pages. The Debian quilt patches
(`fix_path`, `fix_config`, `hardcode_scriptdir_path`, `fix_gcc9`, `fix_gcc10`)
are applied into the source tree as part of the re-home; the button-fix is
committed directly on top. `packaging/deb-version.py` writes `debian/changelog`
at build time.
