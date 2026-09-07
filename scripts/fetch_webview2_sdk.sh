#!/bin/bash
# Pinned build-time headers only; webview supplies the loader implementation.
# The NuGet package's own license travels with the extracted SDK.
set -euo pipefail
sdk_dir=${1:?usage: fetch_webview2_sdk.sh SDK_DIR}
version=1.0.1150.38
sha256=921c004bd1764b585496b2eb3eec0a59a9a98e698246f1d9a3f1c08d1d84ebd5
url="https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$version"
mkdir -p "$sdk_dir"
sdk_dir=$(cd "$sdk_dir" && pwd)
if [ -f "$sdk_dir/.sha256" ] && [ "$(cat "$sdk_dir/.sha256")" = "$sha256" ] &&
   [ -s "$sdk_dir/build/native/include/WebView2.h" ] && [ -s "$sdk_dir/LICENSE.txt" ]; then
	exit 0
fi
work=$(mktemp -d "$sdk_dir/.fetch.XXXXXX")
trap 'rm -rf "$work"' EXIT
curl --fail --location --retry 3 --connect-timeout 15 --max-time 180 "$url" -o "$work/sdk.zip"
printf '%s  %s\n' "$sha256" "$work/sdk.zip" | sha256sum -c -
unzip -q "$work/sdk.zip" 'build/native/include/*' LICENSE.txt -d "$work/extracted"
cp -R "$work/extracted/." "$sdk_dir/"
printf '%s\n' "$sha256" > "$sdk_dir/.sha256"
