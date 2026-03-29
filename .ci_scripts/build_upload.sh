#!/bin/bash

set -e

mkdir deploy
mkdir -p dist
build_dir="$(pwd)/build"

copy_dedicated_scenarios() {
  local source_dir="res/maps"
  local target_dir="$1"
  local copied=0

  mkdir -p "$target_dir"

  while IFS= read -r -d '' file; do
    local header_hex
    header_hex="$(od -An -tx1 -N8 "$file" | tr -d ' \n')"
    if [[ "$header_hex" == "56455253494f4e00" ]]; then
      cp "$file" "$target_dir/$(basename "$file")"
      copied=1
    fi
  done < <(find "$source_dir" -maxdepth 1 -type f \( -name '*.map' -o -name '*.mapx' \) -print0)

  if [[ "$copied" -eq 0 ]]; then
    echo "No standalone versioned scenarios found for dedicated server packaging"
    exit 1
  fi
}

VERSION=$(cat res/version.txt)
if [[ "$GITHUB_REF" =~ ^refs/tags/v ]]
then
  REPO=release
elif [[ "$GITHUB_REF" == "refs/heads/master" ]]
then
  REPO=development
elif [[ "$GITHUB_REF" =~ ^refs/pull/ ]]
then
  PR_ID=${GITHUB_REF##refs/pull/}
  PR_ID=${PR_ID%%/merge}
  VERSION=pr-$PR_ID-$VERSION
else
  echo "Unknown branch type $GITHUB_REF - skipping upload"
fi

DEPLOY_FILE=
UPLOAD_SOURCE=
case "$DEPLOY" in
"linux")
  PACKAGE=linux
  DEPLOY_FILE=claudius-$VERSION-linux-x86_64.tar.gz
  cp "${build_dir}/claudius" "deploy/claudius"
  cp -r "${build_dir}/assets" "deploy/assets"
  cp -r "${build_dir}/maps" "deploy/maps"
  cp -r "${build_dir}/manual" "deploy/manual"
  tar -czf "dist/$DEPLOY_FILE" -C deploy claudius assets maps manual
  UPLOAD_SOURCE="dist/$DEPLOY_FILE"
  ;;
"linux-server")
  PACKAGE=linux-server
  DEPLOY_FILE=claudius-server-$VERSION-linux-x86_64.tar.gz
  cp "${build_dir}/claudius-server" "deploy/claudius-server"
  cp "claudius-server.ini" "deploy/claudius-server.ini"
  copy_dedicated_scenarios "deploy/scenarios"
  mkdir -p "deploy/savegames"
  tar -czf "dist/$DEPLOY_FILE" -C deploy claudius-server claudius-server.ini scenarios savegames
  UPLOAD_SOURCE="dist/$DEPLOY_FILE"
  ;;
"flatpak")
  PACKAGE=linux-flatpak
  DEPLOY_FILE=claudius-$VERSION-linux.flatpak
  flatpak build-export export repo
  flatpak build-bundle export claudius.flatpak com.github.dathannobrega.claudius --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
  cp claudius.flatpak "deploy/$DEPLOY_FILE"
  ;;
"vita")
  PACKAGE=vita
  DEPLOY_FILE=claudius-$VERSION-vita.vpk
  cp "${build_dir}/claudius.vpk" "deploy/$DEPLOY_FILE"
  ;;
"switch")
  PACKAGE=switch
  DEPLOY_FILE=claudius-$VERSION-switch.nro
  cp "${build_dir}/claudius.nro" "deploy/$DEPLOY_FILE"
  ;;
"appimage")
  PACKAGE=linux-appimage
  DEPLOY_FILE=claudius-$VERSION-linux.AppImage
  cp "${build_dir}/claudius.AppImage" "deploy/claudius.AppImage"
  cp -r "${build_dir}/maps" "deploy/maps"
  cp -r "${build_dir}/manual" "deploy/manual"
  ;;
"mac")
  PACKAGE=mac
  DEPLOY_FILE=claudius-$VERSION-mac.dmg
  cp "${build_dir}/claudius.dmg" "deploy/claudius.dmg"
  cp -r "${build_dir}/maps" "deploy/maps"
  cp -r "${build_dir}/manual" "deploy/manual"
  ;;
"emscripten")
  PACKAGE=emscripten
  if [ -f "${build_dir}/claudius.html" ]
  then
    DEPLOY_FILE=claudius-$VERSION-emscripten.html
    cp "${build_dir}/claudius.html" "deploy/$DEPLOY_FILE"
  fi
  ;;
*)
  echo "Unknown deploy type $DEPLOY - skipping upload"
  exit
  ;;
esac

if [ ! -z "$SKIP_UPLOAD" ]
then
  echo "Build is configured to skip deploy - skipping upload"
  exit
fi

if [ -z "$REPO" ] || [ -z "$DEPLOY_FILE" ]
then
  echo "No repo or deploy file found - skipping upload"
  exit
fi

if [ -z "$UPLOAD_SOURCE" ]
then
  UPLOAD_SOURCE="deploy/$DEPLOY_FILE"
fi

if [ -z "$UPLOAD_TOKEN" ]
then
  echo "No upload token found - skipping upload"
  exit
fi

curl -u "$UPLOAD_TOKEN" -T "$UPLOAD_SOURCE" https://claudius.datan.com.br/upload/$REPO/$PACKAGE/$VERSION/$DEPLOY_FILE
echo "Uploaded. URL: https://claudius.datan.com.br/$REPO.html" 
