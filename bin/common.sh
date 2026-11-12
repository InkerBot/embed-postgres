# Sourced by bin/update, bin/apply and bin/generate.  Every path and upstream
# default belongs here: the three scripts once carried their own copies of
# REPO_URL/UPSTREAM_BRANCH and drifted apart.

ROOT="$(dirname "$SCRIPT_DIR")"

POSTGRES="$ROOT/works/postgres"
CORE="$ROOT/works/core"
PATCH_DIR="$ROOT/works/patch"
REV_FILE="$ROOT/works/rev"
IGNORE_FILE="$ROOT/works/ignore"

REPO_URL="https://github.com/postgres/postgres.git"
UPSTREAM_BRANCH="master"

CONFIG="$ROOT/works/config"
if [ -f "$CONFIG" ]; then
    . "$CONFIG"
fi

# works/core is a throwaway repo, so committing in it must not depend on a
# global git identity -- fresh machines and CI containers have none.
core_git() {
    git -C "$CORE" \
        -c user.name="embed-postgres" \
        -c user.email="embed-postgres@localhost" \
        -c commit.gpgsign=false \
        "$@"
}

# Fetches only when the commit is not already present locally.
checkout_upstream_rev() {
    _rev="$1"

    if ! git -C "$POSTGRES" cat-file -e "${_rev}^{commit}" 2>/dev/null; then
        echo "Rev not present locally, fetching..."
        git -C "$POSTGRES" fetch --all
    fi
    git -C "$POSTGRES" checkout --quiet --detach "$_rev"
}
