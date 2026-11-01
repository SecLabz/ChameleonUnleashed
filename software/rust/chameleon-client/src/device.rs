//! Read-only hardware identity and device preferences. Pairing keys are never exposed.
use crate::{Client, Result, command::*};
use std::time::Duration;

#[derive(Default, Clone, Debug, PartialEq, Eq)]
pub struct DeviceSettings {
    pub version: Option<u8>,
    pub animation: Option<u8>,
    /// Short A, short B, long A, long B.
    pub buttons: [Option<u8>; 4],
    pub ble_pairing: Option<bool>,
    pub wake_timeout_s: Option<u8>,
    pub long_press_ms: Option<u16>,
}

#[derive(Default, Clone, Debug, PartialEq, Eq)]
pub struct DeviceIdentity {
    pub chip_id: Option<[u8; 8]>,
    pub ble_address: Option<[u8; 6]>,
}

impl Client {
    fn device_read(&mut self, id: u16, payload: &[u8]) -> Result<Option<Vec<u8>>> {
        if !self.capabilities.contains(&id) {
            return Ok(None);
        }
        match self.command(id, payload, Duration::from_secs(3)) {
            Ok(data) => Ok(Some(data)),
            Err(error)
                if error
                    .downcast_ref::<crate::DeviceError>()
                    .is_some_and(|error| matches!(error.0, 0x67 | 0x69)) =>
            {
                Ok(None)
            }
            Err(error) => Err(error),
        }
    }

    fn device_byte(&mut self, id: u16, payload: &[u8]) -> Result<Option<u8>> {
        self.device_read(id, payload)?
            .map(|data| match data.as_slice() {
                [byte] => Ok(*byte),
                _ => Err(format!("Invalid device setting reply ({id})").into()),
            })
            .transpose()
    }

    pub fn device_settings(&mut self) -> Result<DeviceSettings> {
        let mut settings = DeviceSettings::default();
        if let Some(data) = self.device_read(GET_DEVICE_SETTINGS, &[])? {
            // The firmware's 13-byte envelope includes a six-byte pairing key.
            // Some versions append fields; read timing via dedicated commands.
            if data.len() < 13 || data[6] > 1 {
                return Err("Invalid device settings reply".into());
            }
            settings.version = Some(data[0]);
            settings.animation = Some(data[1]);
            settings.buttons = [Some(data[2]), Some(data[3]), Some(data[4]), Some(data[5])];
            settings.ble_pairing = Some(data[6] != 0);
        } else {
            settings.animation = self.device_byte(GET_ANIMATION, &[])?;
            for (index, (command, button)) in [
                (GET_BUTTON_PRESS, b'A'),
                (GET_BUTTON_PRESS, b'B'),
                (GET_BUTTON_LONG_PRESS, b'A'),
                (GET_BUTTON_LONG_PRESS, b'B'),
            ]
            .into_iter()
            .enumerate()
            {
                settings.buttons[index] = self.device_byte(command, &[button])?;
            }
            settings.ble_pairing = match self.device_byte(GET_BLE_PAIRING, &[])? {
                Some(0) => Some(false),
                Some(1) => Some(true),
                None => None,
                _ => return Err("Invalid BLE pairing reply".into()),
            };
        }
        settings.wake_timeout_s = self.device_byte(GET_SLEEP_TIMEOUT, &[])?;
        settings.long_press_ms = self
            .device_read(GET_LONG_PRESS_THRESHOLD, &[])?
            .map(|data| -> Result<u16> {
                let bytes: [u8; 2] = data
                    .try_into()
                    .map_err(|_| "Invalid long press threshold reply")?;
                Ok(u16::from_be_bytes(bytes))
            })
            .transpose()?;
        Ok(settings)
    }

    pub fn device_identity(&mut self) -> Result<DeviceIdentity> {
        let chip_id = self
            .device_read(DEVICE_CHIP_ID, &[])?
            .map(|data| data.try_into().map_err(|_| "Invalid device chip ID reply"))
            .transpose()?;
        let ble_address = self
            .device_read(DEVICE_ADDRESS, &[])?
            .map(|data| data.try_into().map_err(|_| "Invalid device address reply"))
            .transpose()?;
        Ok(DeviceIdentity {
            chip_id,
            ble_address,
        })
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::test_support::device;

    #[test]
    fn settings_decode_firmware_envelope_and_separate_timing_without_exposing_key() {
        for extra in [vec![], vec![8, 3, 232]] {
            let mut envelope = vec![7, 3, 1, 2, 3, 4, 1];
            envelope.extend(b"654321");
            envelope.extend(extra);
            let (mut client, worker) = device(vec![
                (GET_DEVICE_SETTINGS, vec![], 0x68, envelope),
                (GET_SLEEP_TIMEOUT, vec![], 0x68, vec![8]),
                (GET_LONG_PRESS_THRESHOLD, vec![], 0x68, vec![3, 232]),
            ]);
            client.capabilities = [
                GET_DEVICE_SETTINGS,
                GET_SLEEP_TIMEOUT,
                GET_LONG_PRESS_THRESHOLD,
            ]
            .into();
            let settings = client.device_settings().unwrap();
            assert_eq!(settings.buttons, [Some(1), Some(2), Some(3), Some(4)]);
            assert_eq!(settings.animation, Some(3));
            assert_eq!(settings.ble_pairing, Some(true));
            assert_eq!(settings.wake_timeout_s, Some(8));
            assert_eq!(settings.long_press_ms, Some(1000));
            assert!(!format!("{settings:?}").contains("654321"));
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn missing_capabilities_skip_io_and_individual_settings_support_older_firmware() {
        let (mut client, worker) = device(vec![
            (GET_BUTTON_PRESS, vec![b'A'], 0x68, vec![5]),
            (GET_BUTTON_PRESS, vec![b'B'], 0x68, vec![0]),
        ]);
        client.capabilities = [GET_BUTTON_PRESS].into();
        let settings = client.device_settings().unwrap();
        assert_eq!(settings.buttons, [Some(5), Some(0), None, None]);
        assert_eq!(settings.animation, None);
        assert_eq!(client.device_identity().unwrap(), DeviceIdentity::default());
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn malformed_replies_and_device_errors_are_reported() {
        for (id, status, bytes) in [
            (GET_DEVICE_SETTINGS, 0x68, vec![7; 12]),
            (GET_DEVICE_SETTINGS, 0x68, vec![7; 13]),
            (GET_SLEEP_TIMEOUT, 0x68, vec![8, 8]),
            (GET_LONG_PRESS_THRESHOLD, 0x68, vec![1]),
            (GET_DEVICE_SETTINGS, 0x76, vec![]),
        ] {
            let (mut client, worker) = device(vec![(id, vec![], status, bytes)]);
            client.capabilities = [id].into();
            assert!(client.device_settings().is_err());
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn identity_preserves_network_byte_order() {
        let (mut client, worker) = device(vec![
            (DEVICE_CHIP_ID, vec![], 0x68, vec![1, 2, 3, 4, 5, 6, 7, 8]),
            (DEVICE_ADDRESS, vec![], 0x68, vec![0xc0, 1, 2, 3, 4, 5]),
        ]);
        client.capabilities = [DEVICE_CHIP_ID, DEVICE_ADDRESS].into();
        let identity = client.device_identity().unwrap();
        assert_eq!(identity.chip_id, Some([1, 2, 3, 4, 5, 6, 7, 8]));
        assert_eq!(identity.ble_address, Some([0xc0, 1, 2, 3, 4, 5]));
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn advertised_but_unimplemented_preferences_do_not_hide_the_overview() {
        for status in [0x67, 0x69] {
            let (mut client, worker) = device(vec![
                (GET_DEVICE_SETTINGS, vec![], status, vec![]),
                (GET_ANIMATION, vec![], 0x68, vec![1]),
                (GET_SLEEP_TIMEOUT, vec![], status, vec![]),
            ]);
            client.capabilities = [GET_DEVICE_SETTINGS, GET_ANIMATION, GET_SLEEP_TIMEOUT].into();
            let settings = client.device_settings().unwrap();
            assert_eq!(settings.animation, Some(1));
            assert_eq!(settings.wake_timeout_s, None);
            drop(client);
            worker.join().unwrap();
        }
    }
}
