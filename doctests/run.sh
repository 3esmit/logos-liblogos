#!/usr/bin/env bash
#
# Execute the liblogos doc-tests end-to-end and regenerate their Markdown.
#
# Two specs run, each exercising THIS liblogos commit a different way:
#   - liblogos-module-runtime.test.yaml — drives liblogos through the headless
#     `logoscore` CLI frontend (build logoscore against this liblogos, install
#     the accounts module, start the daemon, call methods).
#   - liblogos-as-a-library.test.yaml — embeds liblogos directly: builds
#     liblogos_core from this commit, compiles a small C++ program that links it,
#     installs modules with lgpm, and loads the accounts module through the C API.
#
# The runner is the shared `doctest` CLI
# (https://github.com/logos-co/logos-doctest), invoked directly via its flake.
# `doctest run` executes every command in a temp directory and asserts on the
# output; `doctest generate` renders the same spec to Markdown under outputs/;
# `doctest clean` strips build artifacts so only the generated docs remain.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

# Run from this doctests/ directory regardless of where the script is invoked from.
cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"
OUTPUT_DIR="./outputs"
# Specs to run. Each renders to outputs/<its `output:` filename>.md; its
# disposable run artifacts go in a per-spec subdir so the two don't collide.
SPECS=(
  "liblogos-module-runtime.test.yaml"
  "liblogos-as-a-library.test.yaml"
)

# Build the doc-test against THIS repo's current commit rather than the latest
# published flake. The spec overrides logoscore's `logos-liblogos` input with
# `github:logos-co/logos-liblogos{release}`, and the pin below makes {release}
# expand to $COMMIT — so the runtime is built against exactly what's checked out
# here. CI rewrites that base URL to the repository under test before applying
# its pin. Override by exporting COMMIT (e.g. a tag), or set COMMIT="" to fall
# back to latest master.
#
# Note: nix fetches the commit from the GitHub remote, so $COMMIT must be pushed
# to logos-co/logos-liblogos. A local-only / uncommitted HEAD won't resolve;
# export COMMIT="" (or push first) in that case.
COMMIT="${COMMIT-$(git rev-parse HEAD)}"
RELEASE_FOR=()
if [ -n "${COMMIT}" ]; then
  RELEASE_FOR=(--release-for "logos-liblogos=${COMMIT}")
  echo "==> Pinning logos-liblogos to ${COMMIT}"
else
  echo "==> COMMIT empty; building against latest logos-liblogos master"
fi

echo "==> Clearing previous ${OUTPUT_DIR}/"
# A prior run copies module artifacts out of the read-only nix store, so the
# directories land read-only (r-x) too. `rm -rf` can't delete files inside a
# directory it can't write to, so restore write permission first.
if [ -e "${OUTPUT_DIR}" ]; then
  chmod -R u+w "${OUTPUT_DIR}" 2>/dev/null || true
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# Read the `output:` filename a spec renders to (e.g. "foo.md") from its YAML.
spec_output() {
  sed -n 's/^output:[[:space:]]*//p' "$1" | head -n1 | tr -d '"'
}

for SPEC in "${SPECS[@]}"; do
  STEM="${SPEC%.test.yaml}"
  MD="$(spec_output "${SPEC}")"
  : "${MD:=${STEM}.md}"
  SPEC_OUT="${OUTPUT_DIR}/${STEM}"

  echo "==> Running ${SPEC} into ${SPEC_OUT}/"
  # ${RELEASE_FOR[@]+...} guards the expansion so an empty array doesn't trip
  # `set -u` on older bash (e.g. macOS's stock 3.2).
  "${DOCTEST[@]}" run "${SPEC}" \
    --verbose \
    --continue-on-fail \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    --output-dir "${SPEC_OUT}/"

  echo "==> Generating ${OUTPUT_DIR}/${MD}"
  "${DOCTEST[@]}" generate "${SPEC}" \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    -o "${OUTPUT_DIR}/${MD}"
done

if [ ! -d "${OUTPUT_DIR}" ]; then
  echo "==> No ${OUTPUT_DIR}/ produced; nothing to clean."
  exit 0
fi

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered docs are in ${OUTPUT_DIR}/."
