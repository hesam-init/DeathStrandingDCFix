#!/usr/bin/env bash
# Tag the current commit and push the tag. The tag triggers .github/workflows/release.yml, which
# builds the MSVC and MinGW zips and publishes them as a GitHub Release.
#
#   ./tag_release.sh 1.2.3        # or v1.2.3; a '-' suffix (1.2.3-rc1) makes a pre-release
#   ./tag_release.sh              # asks for the version

set -euo pipefail

REMOTE=origin
MAIN_BRANCH=main

die() { echo "error: $*" >&2; exit 1; }

confirm() {
    local answer
    read -r -p "$1 [y/N] " answer
    [[ "$answer" =~ ^[Yy]$ ]]
}

cd "$(git rev-parse --show-toplevel)"

TAG="${1:-}"
if [[ -z "$TAG" ]]; then
    echo "Existing tags: $(git tag --sort=-v:refname | head -5 | tr '\n' ' ')"
    read -r -p "Enter release version: " TAG
fi

# Same patterns as the workflow trigger: 1.2.3 or v1.2.3, with an optional suffix.
[[ "$TAG" =~ ^v?[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] \
    || die "'$TAG' is not a version tag (expected 1.2.3, v1.2.3 or 1.2.3-rc1)"

[[ -z "$(git status --porcelain)" ]] || die "working tree has uncommitted changes"

git fetch --quiet --tags "$REMOTE"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag '$TAG' already exists"

BRANCH="$(git branch --show-current)"
if [[ "$BRANCH" != "$MAIN_BRANCH" ]]; then
    confirm "You are on '${BRANCH:-detached HEAD}', not '$MAIN_BRANCH'. Tag this commit anyway?" || exit 1
fi

# The release should point at a commit others can see, not one that only exists locally.
if [[ -z "$(git branch -r --contains HEAD "$REMOTE/*")" ]]; then
    confirm "HEAD isn't on any '$REMOTE' branch yet (push it first?). Tag it anyway?" || exit 1
fi

KIND=release
[[ "$TAG" == *-* ]] && KIND=pre-release

echo
echo "  Tag:    $TAG ($KIND)"
echo "  Commit: $(git log -1 --format='%h %s')"
echo "  Push:   $REMOTE"
echo
confirm "Create and push this tag?" || exit 1

git tag -a "$TAG" -m "$TAG"
if ! git push "$REMOTE" "refs/tags/$TAG"; then
    git tag -d "$TAG" >/dev/null
    die "push failed; the local tag was removed"
fi

echo
echo "Pushed $TAG. The release workflow is starting:"
REPO_URL="$(git remote get-url "$REMOTE" | sed -E 's#^git@([^:]+):#https://\1/#; s#\.git$##')"
echo "  $REPO_URL/actions/workflows/release.yml"
command -v gh >/dev/null && echo "  Watch it with: gh run watch"
