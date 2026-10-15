#!/usr/bin/env bash
# Vendors tomlc17 into components/easytier-esp/third_party/tomlc17.
#
# The upstream public API is renamed with an `et_` prefix so the component's
# private static library cannot collide with an application that links another
# copy of tomlc17 (or any other library defining `toml_*` symbols).
#
# Usage: tools/vendor_tomlc17.sh
set -euo pipefail

COMMIT="fba134d7d1f9b58414f908600b2869f7e55ff2c4"
BASE="https://raw.githubusercontent.com/cktan/tomlc17/${COMMIT}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/components/easytier-esp/third_party/tomlc17"

mkdir -p "${DEST}"
fetch() {
    local url="$1" out="$2"
    for _ in 1 2 3; do
        if curl -fsSL --max-time 60 --retry 2 "${url}" -o "${out}"; then
            return 0
        fi
        sleep 2
    done
    echo "error: failed to fetch ${url}" >&2
    return 1
}
fetch "${BASE}/src/tomlc17.h" "${DEST}/tomlc17.h"
fetch "${BASE}/src/tomlc17.c" "${DEST}/tomlc17.c"
fetch "${BASE}/LICENSE" "${DEST}/LICENSE"

# Rename only the public function symbols (types/constants are compile-time and
# cannot collide across translation units).
for f in "${DEST}/tomlc17.h" "${DEST}/tomlc17.c"; do
    sed -i -E \
        's/\btoml_(parse_file_ex|parse_file_named|parse_file|parse_named|parse|free|get|seek|merge|equiv|default_option|set_option|table_find)\b/et_toml_\1/g' \
        "$f"
done

cat > "${DEST}/README.md" <<EOF
# tomlc17 (vendored)

- Upstream: https://github.com/cktan/tomlc17
- Commit: \`${COMMIT}\`
- License: MIT (see LICENSE)
- Vendored by: \`tools/vendor_tomlc17.sh\`

Local modification: every public function symbol is renamed with an \`et_\`
prefix (\`toml_parse\` -> \`et_toml_parse\`, ...) to keep the component's copy
private to its static library. No parsing behaviour is changed.
EOF

echo "vendored tomlc17 @ ${COMMIT} into ${DEST}"
