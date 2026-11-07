#!/bin/env bash

if [[ $BASH_SOURCE = */* ]]; then
  cd -- "${BASH_SOURCE%/*}/" || exit
fi

if [[ $# -gt 1 || ( $# -eq 1 && $1 != --clean ) ]]; then
  echo "Usage: $0 [--clean]" >&2
  exit 2
fi

softdevice=s140
softdevice_version=7.2.0
softdevice_id=0x0100


# Keep the application version independent from the bootloader counter.
application_version=$(python3 ../resource/tools/project_version.py --dfu) || exit 1
# Stock devices can already carry bootloader version 3. The migration must
# advance that counter; the new permanent web ABI uses layout version 2.
bootloader_version=5

device_type=${CURRENT_DEVICE_TYPE:-ultra}
case $device_type in
  "ultra") hw_version=0 ;;
  *)       echo "The application firmware requires Chameleon Ultra, aborting."; exit 1 ;;
esac

if [[ ! -f ../.cache/web/assets/web_assets.h ]]; then
  echo "Prepare the web assets first; see software/web/README.md."
  exit 1
fi
echo "Building firmware for $device_type (hw_version=$hw_version)"

set -xe

if [[ ${1:-} == --clean ]]; then
  rm -rf "objects"
fi

# Keep compiler outputs, but never update packages or merged images from an older build.
rm -f objects/chameleon-unleashed-app.zip objects/chameleon-unleashed-full.zip objects/chameleon-unleashed-install.zip objects/chameleon-unleashed-bootloader.zip \
  objects/chameleon-unleashed-binaries.zip objects/settings.hex \
  objects/application_merged.hex objects/fullimage.hex objects/chameleon-unleashed.cuw

(
  cd bootloader
  make -j
)

(
  cd application
  make -j
)

python3 ../resource/tools/build_recovery.py
(
  cd recovery
  make -j
)
python3 ../resource/tools/web_update.py --bootloader-abi
python3 ../resource/tools/web_update.py

(
  cd objects

  cp ../nrf52_sdk/components/softdevice/${softdevice}/hex/${softdevice}_nrf52_${softdevice_version}_softdevice.hex softdevice.hex
  cp ../../.cache/web/assets/THIRD_PARTY.txt THIRD_PARTY.txt
  
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version $hw_version \
    --bootloader  bootloader.hex   --bootloader-version  $bootloader_version  --key-file ../../resource/dfu_key/chameleon.pem \
    --application installation.hex  --application-version $application_version --app-boot-validation VALIDATE_GENERATED_CRC \
    --softdevice  softdevice.hex \
    --sd-req ${softdevice_id} --sd-id ${softdevice_id} \
    chameleon-unleashed-full.zip
	
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version $hw_version --key-file ../../resource/dfu_key/chameleon.pem \
    --application installation.hex  --application-version $application_version --app-boot-validation VALIDATE_GENERATED_CRC \
    --sd-req ${softdevice_id} \
    chameleon-unleashed-app.zip

  # One-time migration: bootloader first, then recovery + app; never stage the
  # much larger SoftDevice with an old bootloader that only reserves 12 KiB.
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version $hw_version --key-file ../../resource/dfu_key/chameleon.pem \
    --bootloader bootloader.hex --bootloader-version $bootloader_version \
    --sd-req ${softdevice_id} \
    chameleon-unleashed-bootloader.zip
  python3 - <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, '../../resource/tools')
from web_update import installation_package
installation_package(Path('chameleon-unleashed-bootloader.zip'),
                     Path('chameleon-unleashed-app.zip'), Path('chameleon-unleashed-install.zip'))
PY

  zip chameleon-unleashed-full.zip THIRD_PARTY.txt
  zip chameleon-unleashed-app.zip THIRD_PARTY.txt
  zip chameleon-unleashed-install.zip THIRD_PARTY.txt
  zip chameleon-unleashed-bootloader.zip THIRD_PARTY.txt

  python3 ../../resource/tools/web_update.py --publish

  nrfutil nrf5sdk-tools settings generate \
    --family NRF52840 \
    --application installation.hex --application-version $application_version \
    --softdevice softdevice.hex \
    --bootloader-version $bootloader_version --bl-settings-version 2 \
    settings.hex
  mergehex \
    --merge \
    settings.hex \
    installation.hex \
    --output application_merged.hex

  mergehex \
    --merge \
      bootloader.hex \
      application_merged.hex \
      softdevice.hex \
    --output fullimage.hex

  tmp_dir=$(mktemp -d -t cu_binaries_XXXXXXXXXX)
  trap 'rm -rf "$tmp_dir"' EXIT
  cp application_merged.hex "$tmp_dir/application.hex"
  cp bootloader.hex fullimage.hex softdevice.hex "$tmp_dir"
  zip -j chameleon-unleashed-binaries.zip "$tmp_dir"/*.hex THIRD_PARTY.txt
)
