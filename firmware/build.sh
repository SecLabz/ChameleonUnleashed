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


# TODO: find a way to manage this automatically, I don't want to rely on action build #.
application_version=1
bootloader_version=1

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
rm -f objects/chameleon-unleashed-app.zip objects/chameleon-unleashed-full.zip \
  objects/chameleon-unleashed-binaries.zip objects/settings.hex \
  objects/application_merged.hex objects/fullimage.hex

(
  cd bootloader
  make -j
)

(
  cd application
  make -j
)

(
  cd objects

  cp ../nrf52_sdk/components/softdevice/${softdevice}/hex/${softdevice}_nrf52_${softdevice_version}_softdevice.hex softdevice.hex
  cp ../../.cache/web/assets/THIRD_PARTY.txt THIRD_PARTY.txt
  
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version $hw_version \
    --bootloader  bootloader.hex   --bootloader-version  $bootloader_version  --key-file ../../resource/dfu_key/chameleon.pem \
    --application application.hex  --application-version $application_version --app-boot-validation VALIDATE_GENERATED_CRC \
    --softdevice  softdevice.hex \
    --sd-req ${softdevice_id} --sd-id ${softdevice_id} \
    chameleon-unleashed-full.zip
	
  nrfutil nrf5sdk-tools pkg generate \
    --hw-version $hw_version --key-file ../../resource/dfu_key/chameleon.pem \
    --application application.hex  --application-version $application_version --app-boot-validation VALIDATE_GENERATED_CRC \
    --sd-req ${softdevice_id} \
    chameleon-unleashed-app.zip

  zip chameleon-unleashed-full.zip THIRD_PARTY.txt
  zip chameleon-unleashed-app.zip THIRD_PARTY.txt

  nrfutil nrf5sdk-tools settings generate \
    --family NRF52840 \
    --application application.hex --application-version $application_version \
    --softdevice softdevice.hex \
    --bootloader-version $bootloader_version --bl-settings-version 2 \
    settings.hex
  mergehex \
    --merge \
    settings.hex \
    application.hex \
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
