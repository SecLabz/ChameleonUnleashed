//! UID-checked Type 2 reads under a bounded RF lease.
use crate::{DeviceError, Reader, Result, TagCredential, command::*};
use nfc_native::formats::image::Model;
use std::time::{Duration, Instant};

impl Reader<'_> {
    pub fn type2_pages_with_password(
        &mut self,
        model: Model,
        page: u8,
        password: [u8; 4],
    ) -> Result<[u8; 16]> {
        self.type2_pages_with_credential(model, page, TagCredential::Password(password))
    }

    /// Authenticate for this UID-checked read only. READ may wrap after its first page.
    pub fn type2_pages_with_credential(
        &mut self,
        model: Model,
        page: u8,
        credential: TagCredential,
    ) -> Result<[u8; 16]> {
        let limit = if model == Model::UltralightC {
            44 // Key pages are never readable over RF.
        } else {
            model.type2().map_or(0, |(pages, _)| pages)
        };
        let valid_credential = match credential {
            TagCredential::None => true,
            TagCredential::Password(_) => model != Model::UltralightC,
            TagCredential::UltralightC(_) => model == Model::UltralightC,
            _ => false,
        };
        if self.card.sak != 0
            || self.card.uid.len() != 7
            || usize::from(page) >= limit
            || !valid_credential
            || !self.client.capabilities.contains(&TYPE2_AUTH_READ)
        {
            return Err("Authenticated Type 2 read is unsupported or invalid".into());
        }
        self.close_session()?;
        let mut request = [0u8; 27];
        request[0] = model as u8;
        request[1] = 7;
        request[2..9].copy_from_slice(&self.card.uid);
        request[9] = page;
        let (kind, key) = credential.wire();
        request[10] = kind;
        request[11..27].copy_from_slice(&key);
        let bytes = self
            .client
            .command(TYPE2_AUTH_READ, &request, Duration::from_secs(3))
            .map_err(|error| {
                if model == Model::UltralightC
                    && error
                        .downcast_ref::<DeviceError>()
                        .is_some_and(|error| matches!(error.0, 0x60 | 0x67 | 0x69))
                {
                    return "Update the firmware for Ultralight C physical restoration".into();
                }
                error
            })?;
        bytes
            .try_into()
            .map_err(|_| "Invalid authenticated Type 2 reply".into())
    }

    pub fn type2_model(&mut self, selected: Option<Model>) -> Result<Model> {
        if self.card.sak != 0 || self.card.uid.len() != 7 {
            return Err("Expected a seven-byte Type 2 tag".into());
        }
        if let Some(model) = selected {
            if model.type2().is_none() {
                return Err("Expected a Type 2 model".into());
            }
            return Ok(model);
        }
        let version = self.type2_version().map_err(|error| {
            format!("Cannot identify the Type 2 model: {error}. Supply --model for original Ultralight or Ultralight C")
        })?;
        let profile = nfc_native::formats::type2::from_version(&version)
            .ok_or("Unknown Type 2 version; supply a supported --model")?;
        (1..=22)
            .filter_map(|id| Model::from_id(id).ok())
            .find(|model| {
                model
                    .type2_profile()
                    .is_some_and(|value| value.name == profile.name)
            })
            .ok_or_else(|| "Unsupported Type 2 model".into())
    }

    /// Only the first page of READ is trusted: protected ranges can wrap.
    pub fn type2_page(
        &mut self,
        model: Model,
        page: u8,
        credential: TagCredential,
    ) -> Result<[u8; 4]> {
        let pages = model.type2().ok_or("Expected a Type 2 model")?.0;
        if usize::from(page) >= pages
            || model
                .type2_profile()
                .unwrap()
                .credential_pages()
                .contains(&usize::from(page))
        {
            return Err("Page is outside readable memory or contains hidden credentials".into());
        }
        let bytes = if credential == TagCredential::None {
            self.type2_pages(page)?
        } else {
            self.type2_pages_with_credential(model, page, credential)?
        };
        Ok(bytes[..4].try_into().unwrap())
    }

    pub fn type2_pages(&mut self, page: u8) -> Result<[u8; 16]> {
        Ok(self.type2_request(1, page, None, 16)?.try_into().unwrap())
    }

    pub fn type2_version(&mut self) -> Result<[u8; 8]> {
        Ok(self.type2_request(0, 0, None, 8)?.try_into().unwrap())
    }

    /// Read public NTAG pages: exactly `count * 4` bytes without READ's wrapping.
    pub fn type2_fast_pages(&mut self, model: Model, first: u8, count: u8) -> Result<Vec<u8>> {
        if !matches!(model, Model::Ntag213 | Model::Ntag215 | Model::Ntag216)
            || !(1..=8).contains(&count)
            || usize::from(first) + usize::from(count) > model.type2().unwrap().0
        {
            return Err("Invalid NTAG FAST_READ range".into());
        }
        match self.type2_request(5, first, Some(first + count - 1), usize::from(count) * 4) {
            Ok(bytes) => return Ok(bytes),
            Err(error)
                if error
                    .downcast_ref::<DeviceError>()
                    .is_some_and(|error| matches!(error.0, 0x60 | 0x69)) => {}
            Err(error) => return Err(error),
        }
        // Older firmware advertises TYPE2_READ without the FAST_READ operation.
        let mut bytes = Vec::with_capacity(usize::from(count) * 4);
        for page in first..first + count {
            // Only the first page is trustworthy when READ wraps at a protected boundary.
            bytes.extend(&self.type2_pages(page)?[..4]);
        }
        Ok(bytes)
    }

    fn type2_request(
        &mut self,
        operation: u8,
        page: u8,
        end: Option<u8>,
        expected: usize,
    ) -> Result<Vec<u8>> {
        if self.card.sak != 0 || self.card.uid.len() != 7 {
            return Err("Expected a seven-byte Type 2 tag".into());
        }
        let result = (|| {
            if self.reading_memory
                || self
                    .session
                    .is_some_and(|last| last.elapsed() >= Duration::from_secs(1))
            {
                self.close_session()?;
            }
            let keep_field = self.client.capabilities.contains(&NFCA_SESSION);
            if keep_field && self.session.is_none() {
                // Record cleanup before sending, including malformed replies.
                self.session = Some(Instant::now());
                let reply = self
                    .client
                    .command(NFCA_SESSION, &[1], Duration::from_secs(3))?;
                if !reply.is_empty() {
                    return Err("Invalid reader session reply".into());
                }
            }
            let mut request = vec![operation, page, 7];
            request.extend(&self.card.uid);
            request.extend(end);
            let bytes = self
                .client
                .command(TYPE2_READ, &request, Duration::from_secs(3))?;
            if bytes.len() != expected {
                return Err("Invalid Type 2 read reply".into());
            }
            if keep_field {
                self.session = Some(Instant::now());
            }
            Ok(bytes)
        })();
        if result.is_err() {
            let cleanup = self.close_session();
            return crate::with_cleanup(result, cleanup);
        }
        result
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::super::tests::reader;
    use super::*;
    use crate::test_support::{device, device_disconnect, device_with_hook};
    use std::sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    };

    fn request(operation: u8, page: u8, end: Option<u8>) -> Vec<u8> {
        let mut bytes = vec![operation, page, 7, 4, 1, 2, 3, 4, 5, 6];
        bytes.extend(end);
        bytes
    }
    fn type2_reader(client: &mut crate::Client) -> Reader<'_> {
        client.capabilities.insert(NFCA_SESSION);
        let mut reader = reader(client);
        reader.card.sak = 0;
        reader.card.uid = vec![4, 1, 2, 3, 4, 5, 6];
        reader
    }

    #[test]
    fn ultralight_c_reads_encode_the_full_target_key_and_preserve_wrapping() {
        let key = std::array::from_fn(|i| i as u8);
        let mut payload = vec![19, 7, 4, 1, 2, 3, 4, 5, 6, 43, 4];
        payload.extend(key);
        let bytes = [vec![43; 4], vec![0; 4], vec![1; 4], vec![2; 4]].concat();
        let (mut client, worker) = device(vec![(TYPE2_AUTH_READ, payload, 0, bytes.clone())]);
        client.capabilities.insert(TYPE2_AUTH_READ);
        {
            let mut reader = type2_reader(&mut client);
            assert_eq!(
                reader
                    .type2_pages_with_credential(
                        Model::UltralightC,
                        43,
                        TagCredential::UltralightC(key)
                    )
                    .unwrap()
                    .to_vec(),
                bytes
            );
            assert!(
                reader
                    .type2_pages_with_credential(
                        Model::UltralightC,
                        44,
                        TagCredential::UltralightC(key)
                    )
                    .is_err()
            );
            assert!(
                reader
                    .type2_pages_with_password(Model::UltralightC, 4, [0; 4])
                    .is_err()
            );
            assert!(
                reader
                    .type2_pages_with_credential(Model::Ntag213, 4, TagCredential::UltralightC(key))
                    .is_err()
            );
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn ultralight_c_read_authentication_failures_are_not_retried() {
        let mut payload = vec![19, 7, 4, 1, 2, 3, 4, 5, 6, 4, 4];
        payload.extend([7; 16]);
        let (mut client, worker) = device(vec![(TYPE2_AUTH_READ, payload, 6, vec![])]);
        client.capabilities.insert(TYPE2_AUTH_READ);
        {
            let mut reader = type2_reader(&mut client);
            let error = reader
                .type2_pages_with_credential(
                    Model::UltralightC,
                    4,
                    TagCredential::UltralightC([7; 16]),
                )
                .unwrap_err();
            assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 6);
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn authenticated_type2_reads_preserve_completed_data_then_honor_cancellation() {
        let flag = Arc::new(AtomicBool::new(false));
        let cancel = flag.clone();
        let mut payload = vec![19, 7, 4, 1, 2, 3, 4, 5, 6, 4, 4];
        payload.extend([7; 16]);
        let (mut client, worker) = device_with_hook(
            vec![(TYPE2_AUTH_READ, payload, 0, vec![9; 16])],
            move |_| {
                cancel.store(true, Ordering::Relaxed);
            },
        );
        client.capabilities.insert(TYPE2_AUTH_READ);
        client.cancelled = flag;
        {
            let mut reader = type2_reader(&mut client);
            assert_eq!(
                reader
                    .type2_pages_with_credential(
                        Model::UltralightC,
                        4,
                        TagCredential::UltralightC([7; 16])
                    )
                    .unwrap(),
                [9; 16]
            );
            assert!(
                reader
                    .type2_pages_with_credential(
                        Model::UltralightC,
                        5,
                        TagCredential::UltralightC([7; 16])
                    )
                    .unwrap_err()
                    .downcast_ref::<crate::RequestCancelled>()
                    .is_some()
            );
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn password_reads_keep_the_existing_request_and_old_firmware_is_explicit() {
        for (model, credential, payload, status, reply) in [
            (
                Model::Ntag213,
                TagCredential::Password([1, 2, 3, 4]),
                [
                    vec![8, 7, 4, 1, 2, 3, 4, 5, 6, 4, 3],
                    vec![1, 2, 3, 4],
                    vec![0; 12],
                ]
                .concat(),
                0,
                vec![8; 16],
            ),
            (
                Model::UltralightC,
                TagCredential::None,
                [vec![19, 7, 4, 1, 2, 3, 4, 5, 6, 4, 0], vec![0; 16]].concat(),
                0x60,
                vec![],
            ),
        ] {
            let (mut client, worker) = device(vec![(TYPE2_AUTH_READ, payload, status, reply)]);
            client.capabilities.insert(TYPE2_AUTH_READ);
            {
                let mut reader = type2_reader(&mut client);
                let result = reader.type2_pages_with_credential(model, 4, credential);
                if status == 0 {
                    assert_eq!(result.unwrap(), [8; 16]);
                } else {
                    assert!(
                        result
                            .unwrap_err()
                            .to_string()
                            .contains("Update the firmware")
                    );
                }
            }
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn ultralight_c_writes_keep_the_existing_verified_one_shot_wire_contract() {
        let key = [7; 16];
        let unit = nfc_native::formats::physical::WriteUnit {
            index: 39,
            before: vec![0; 4],
            after: vec![1; 4],
        };
        let mut payload = vec![19, 7, 4, 1, 2, 3, 4, 5, 6, 39, 4];
        payload.extend(key);
        payload.extend([0; 16]);
        payload.extend([1; 4]);
        payload.extend([0; 12]);
        let (mut client, worker) = device(vec![(NFCA_WRITE_USER_UNIT, payload, 0, vec![1; 4])]);
        client.capabilities.insert(NFCA_WRITE_USER_UNIT);
        {
            let mut reader = type2_reader(&mut client);
            reader
                .write_user_unit(Model::UltralightC, &unit, TagCredential::UltralightC(key))
                .unwrap();
            assert!(
                reader
                    .write_user_unit(Model::UltralightC, &unit, TagCredential::Password([0; 4]))
                    .is_err()
            );
            let forbidden = nfc_native::formats::physical::WriteUnit { index: 40, ..unit };
            assert!(
                reader
                    .write_user_unit(
                        Model::UltralightC,
                        &forbidden,
                        TagCredential::UltralightC(key)
                    )
                    .is_err()
            );
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn legacy_reads_do_not_require_a_reader_session_command() {
        let (mut client, worker) = device(vec![(TYPE2_READ, request(0, 0, None), 0, vec![7; 8])]);
        {
            let mut reader = type2_reader(&mut client);
            reader.client.capabilities.remove(&NFCA_SESSION);
            assert_eq!(reader.type2_version().unwrap(), [7; 8]);
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn legacy_fast_read_rejections_use_read_without_wrapping_the_last_pages() {
        for status in [0x60, 0x69] {
            let (mut client, worker) = device(vec![
                (NFCA_SESSION, vec![1], 0, vec![]),
                (TYPE2_READ, request(5, 42, Some(44)), status, vec![]),
                (NFCA_SESSION, vec![0], 0, vec![]),
                (NFCA_SESSION, vec![1], 0, vec![]),
                (
                    TYPE2_READ,
                    request(1, 42, None),
                    0,
                    [vec![42; 4], vec![43; 4], vec![44; 4], vec![0; 4]].concat(),
                ),
                (
                    TYPE2_READ,
                    request(1, 43, None),
                    0,
                    [vec![43; 4], vec![44; 4], vec![0; 4], vec![1; 4]].concat(),
                ),
                (
                    TYPE2_READ,
                    request(1, 44, None),
                    0,
                    [vec![44; 4], vec![0; 4], vec![1; 4], vec![2; 4]].concat(),
                ),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ]);
            {
                let mut reader = type2_reader(&mut client);
                assert_eq!(
                    reader.type2_fast_pages(Model::Ntag213, 42, 3).unwrap(),
                    [vec![42; 4], vec![43; 4], vec![44; 4]].concat()
                );
            }
            drop(client);
            worker.join().unwrap();
        }
    }

    #[test]
    fn legacy_read_does_not_label_wrapped_public_pages_as_protected_memory() {
        let (mut client, worker) = device(vec![
            (NFCA_SESSION, vec![1], 0, vec![]),
            (TYPE2_READ, request(5, 8, Some(11)), 0x60, vec![]),
            (NFCA_SESSION, vec![0], 0, vec![]),
            (NFCA_SESSION, vec![1], 0, vec![]),
            (
                TYPE2_READ,
                request(1, 8, None),
                0,
                [vec![8; 4], vec![9; 4], vec![0; 4], vec![1; 4]].concat(),
            ),
            (
                TYPE2_READ,
                request(1, 9, None),
                0,
                [vec![9; 4], vec![0; 4], vec![1; 4], vec![2; 4]].concat(),
            ),
            (TYPE2_READ, request(1, 10, None), 6, vec![]),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        {
            let mut reader = type2_reader(&mut client);
            let error = reader.type2_fast_pages(Model::Ntag213, 8, 4).unwrap_err();
            assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 6);
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn fast_read_card_and_transport_errors_do_not_retry_with_read() {
        let (mut client, worker) = device(vec![
            (NFCA_SESSION, vec![1], 0, vec![]),
            (TYPE2_READ, request(5, 0, Some(7)), 2, vec![]),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        {
            let mut reader = type2_reader(&mut client);
            let error = reader.type2_fast_pages(Model::Ntag213, 0, 8).unwrap_err();
            assert_eq!(error.downcast_ref::<DeviceError>().unwrap().0, 2);
        }
        drop(client);
        worker.join().unwrap();

        let (mut client, worker) = device_disconnect(
            vec![(NFCA_SESSION, vec![1], 0, vec![])],
            TYPE2_READ,
            request(5, 0, Some(7)),
        );
        assert!(
            type2_reader(&mut client)
                .type2_fast_pages(Model::Ntag213, 0, 8)
                .is_err()
        );
        assert!(!client.usable);
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn version_and_fast_ranges_reuse_one_bounded_lease() {
        let (mut client, worker) = device(vec![
            (NFCA_SESSION, vec![1], 0, vec![]),
            (TYPE2_READ, request(0, 0, None), 0, vec![0; 8]),
            (TYPE2_READ, request(5, 0, Some(7)), 0, vec![7; 32]),
            (TYPE2_READ, request(5, 40, Some(42)), 0, vec![42; 12]),
            (NFCA_SESSION, vec![0], 0, vec![]),
        ]);
        {
            let mut reader = type2_reader(&mut client);
            assert_eq!(reader.type2_version().unwrap(), [0; 8]);
            assert_eq!(
                reader.type2_fast_pages(Model::Ntag213, 0, 8).unwrap(),
                vec![7; 32]
            );
            assert_eq!(
                reader.type2_fast_pages(Model::Ntag213, 40, 3).unwrap(),
                vec![42; 12]
            );
            for (model, page, count) in [
                (Model::Ntag213, 44, 2),
                (Model::Ntag216, 230, 2),
                (Model::Ntag213, 0, 0),
                (Model::Ntag213, 0, 9),
                (Model::Ultralight, 0, 4),
            ] {
                assert!(reader.type2_fast_pages(model, page, count).is_err());
            }
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn expiry_and_read_errors_release_the_lease_before_restarting() {
        let mut exchanges = Vec::new();
        for reply in [vec![7; 32], vec![8; 32], vec![0; 31]] {
            exchanges.extend([
                (NFCA_SESSION, vec![1], 0, vec![]),
                (TYPE2_READ, request(5, 0, Some(7)), 0, reply),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ]);
        }
        let (mut client, worker) = device(exchanges);
        {
            let mut reader = type2_reader(&mut client);
            reader.type2_fast_pages(Model::Ntag213, 0, 8).unwrap();
            reader.session = Some(Instant::now() - Duration::from_secs(2));
            reader.type2_fast_pages(Model::Ntag213, 0, 8).unwrap();
            reader.session = Some(Instant::now() - Duration::from_secs(2));
            assert!(reader.type2_fast_pages(Model::Ntag213, 0, 8).is_err());
        }
        drop(client);
        worker.join().unwrap();
    }

    #[test]
    fn cancellation_keeps_a_completed_range_and_closes_rf() {
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();
        let (mut client, worker) = device_with_hook(
            vec![
                (NFCA_SESSION, vec![1], 0, vec![]),
                (TYPE2_READ, request(5, 0, Some(7)), 0, vec![7; 32]),
                (NFCA_SESSION, vec![0], 0, vec![]),
            ],
            move |command| {
                if command == TYPE2_READ {
                    flag.store(true, Ordering::Relaxed);
                }
            },
        );
        client.cancelled = cancelled;
        {
            let mut reader = type2_reader(&mut client);
            assert_eq!(
                reader.type2_fast_pages(Model::Ntag213, 0, 8).unwrap(),
                vec![7; 32]
            );
            assert!(reader.type2_fast_pages(Model::Ntag213, 8, 8).is_err());
        }
        drop(client);
        worker.join().unwrap();
    }
}
