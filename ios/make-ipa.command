#!/bin/bash
# make-ipa.command -- build BaldiCpp for an iOS device and package it as BaldiCpp.ipa.
# Double-click it in Finder (it opens in Terminal), or run:  bash make-ipa.command
#
# For a JAILBROKEN device: the app is built UNSIGNED (no Apple account or certificate needed).
# If `ldid` is installed it is fake-signed as well, which most jailbreak installers want; with
# AppSync Unified on the device the .ipa then installs with Filza / AppInstaller / Cydia Impactor.
# Works with the Xcode 4.5 - 5.1.1 that runs on OS X 10.8 (bash 3.2, xcodebuild, zip).

set -e
cd "$(dirname "$0")"                      # the BaldiCpp-iOS folder (this script sits next to the project)
HERE="$(pwd)"
APP="$HERE/build/Release-iphoneos/BaldiCpp.app"
IPA="$HERE/BaldiCpp.ipa"

echo "== Building BaldiCpp (Release, iOS device, unsigned) =="
echo "   folder: $HERE"
xcodebuild -project BaldiCpp.xcodeproj -target BaldiCpp -configuration Release -sdk iphoneos \
  CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO \
  SYMROOT="$HERE/build" build | grep -E "error|warning: no|BUILD|\*\*" || true
# xcodebuild's own exit status is hidden by the filter above, so check the result directly
if [ ! -x "$APP/BaldiCpp" ]; then
  echo
  echo "BUILD FAILED -- no $APP/BaldiCpp."
  echo "Run this for the full log:  xcodebuild -project BaldiCpp.xcodeproj -configuration Release -sdk iphoneos CODE_SIGNING_REQUIRED=NO build"
  read -p "Press Return to close." x
  exit 1
fi

if command -v ldid >/dev/null 2>&1; then
  echo "== Fake-signing with ldid =="
  ldid -S "$APP/BaldiCpp"
else
  echo "== ldid not found: the app stays unsigned (fine with AppSync Unified; install ldid to fake-sign) =="
fi

echo "== Packaging $IPA =="
rm -rf "$HERE/Payload" "$IPA"
mkdir "$HERE/Payload"
cp -R "$APP" "$HERE/Payload/"
( cd "$HERE" && zip -qry "$IPA" Payload )   # -y keeps symlinks as links
rm -rf "$HERE/Payload"

echo
echo "DONE: $IPA ($(du -h "$IPA" | cut -f1))"
echo "Copy it to the device (e.g. with Filza / AirDrop / iFunBox) and install it."
read -p "Press Return to close." x
