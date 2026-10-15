# tomlc17 (vendored)

- Upstream: https://github.com/cktan/tomlc17
- Commit: `fba134d7d1f9b58414f908600b2869f7e55ff2c4`
- License: MIT (see LICENSE)
- Vendored by: `tools/vendor_tomlc17.sh`

Local modification: every public function symbol is renamed with an `et_`
prefix (`toml_parse` -> `et_toml_parse`, ...) so this private copy cannot
collide with another TOML parser linked by the application. No parsing
behaviour is changed.

Note: the fetches that produced these files used the commit above; re-running
`tools/vendor_tomlc17.sh` reproduces the rename mechanically.
