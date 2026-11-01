use super::*;
use serialport::UsbPortInfo;

#[cfg(target_os = "macos")]
#[test]
fn disconnect_wait_ignores_pending_input_and_observes_tty_hangup() {
    use std::io::Write;

    let (mut master, slave) = serialport::TTYPort::pair().unwrap();
    master.write_all(b"pending response").unwrap();
    assert!(!wait_usb_disconnect(&slave, Duration::ZERO).unwrap());
    // Let the waiter start before the peer disconnects, as during a USB reset.
    let disconnect = thread::spawn(move || {
        thread::sleep(Duration::from_millis(20));
        drop(master);
    });
    let disconnected = wait_usb_disconnect(&slave, Duration::from_secs(1)).unwrap();
    disconnect.join().unwrap();
    assert!(disconnected);
}

fn port(name: &str, id: (u16, u16), serial: Option<&str>) -> SerialPortInfo {
    SerialPortInfo {
        port_name: name.into(),
        port_type: SerialPortType::UsbPort(UsbPortInfo {
            vid: id.0,
            pid: id.1,
            serial_number: serial.map(String::from),
            manufacturer: None,
            product: Some("ChameleonUltra".into()),
        }),
    }
}

#[test]
fn discovery_selects_explicit_ports_and_rejects_ambiguous_or_unrelated_devices() {
    let application = port("/dev/cu.Ultra", APPLICATION_USB_ID, Some("AABBCC001122"));
    let other = port("/dev/cu.Other", APPLICATION_USB_ID, Some("DDEEFF001122"));
    assert!(select(vec![application.clone(), other.clone()], None).is_err());
    assert_eq!(
        select(vec![application.clone(), other], Some("/dev/cu.Ultra"))
            .unwrap()
            .port_name,
        application.port_name
    );
    assert!(select(vec![application], Some("/dev/cu.Missing")).is_err());
    assert!(
        select(
            vec![port("/dev/cu.Unrelated", (0x1234, 0x5678), None)],
            None
        )
        .is_err()
    );
    assert!(select(vec![], None).is_err());
}

#[test]
fn reboot_follows_the_same_serial_number_on_a_new_bootloader_port() {
    let application = port("/dev/cu.Ultra", APPLICATION_USB_ID, Some("AABBCC001122"));
    let bootloader = port("/dev/cu.Dfu", BOOTLOADER_USB_ID, Some("aabbcc001122"));
    let unrelated = port("/dev/cu.OtherDfu", BOOTLOADER_USB_ID, Some("DDEEFF001122"));
    let ports = vec![application, unrelated.clone(), bootloader.clone()];
    assert_eq!(
        matching_port(ports, "AABBCC001122", Some(BOOTLOADER_USB_ID))
            .unwrap()
            .unwrap()
            .port_name,
        bootloader.port_name
    );
    assert!(
        matching_port(vec![unrelated], "AABBCC001122", Some(BOOTLOADER_USB_ID))
            .unwrap()
            .is_none()
    );
    assert!(
        matching_port(
            vec![port("/dev/cu.NoSerial", BOOTLOADER_USB_ID, None)],
            "AABBCC001122",
            Some(BOOTLOADER_USB_ID)
        )
        .unwrap()
        .is_none()
    );
    assert!(
        matching_port(
            vec![bootloader.clone(), bootloader],
            "AABBCC001122",
            Some(BOOTLOADER_USB_ID)
        )
        .is_err()
    );
}

#[test]
fn lite_is_rejected_and_cancellation_prevents_discovery() {
    let mut lite = port("/dev/cu.Lite", APPLICATION_USB_ID, Some("AABBCC001122"));
    if let SerialPortType::UsbPort(info) = &mut lite.port_type {
        info.product = Some("ChameleonLite".into());
    }
    assert!(
        select(vec![lite], None)
            .err()
            .unwrap()
            .to_string()
            .contains("Ultra only")
    );
    let error = Device::discover(None, Arc::new(AtomicBool::new(true)))
        .err()
        .unwrap();
    assert!(error.is::<RequestCancelled>());
}

#[test]
fn a_bootloader_timeout_can_be_followed_back_to_the_same_application() {
    let application = port(
        "/dev/cu.Reconnected",
        APPLICATION_USB_ID,
        Some("AABBCC001122"),
    );
    assert!(
        matching_port(
            vec![application.clone()],
            "AABBCC001122",
            Some(BOOTLOADER_USB_ID)
        )
        .unwrap()
        .is_none()
    );
    assert_eq!(
        matching_port(vec![application], "AABBCC001122", None)
            .unwrap()
            .unwrap()
            .port_name,
        "/dev/cu.Reconnected"
    );
    assert_eq!(
        frame::encode(ENTER_BOOTLOADER, 0, &[]).unwrap(),
        [0x11, 0xef, 0x03, 0xf2, 0, 0, 0, 0, 0x0b, 0]
    );
}

#[test]
fn startup_wait_ignores_dfu_and_other_serial_numbers() {
    let cancelled = AtomicBool::new(false);
    let bootloader = port("/dev/cu.Dfu", BOOTLOADER_USB_ID, Some("AABBCC001122"));
    let unrelated = port("/dev/cu.Other", APPLICATION_USB_ID, Some("DDEEFF001122"));
    let application = port("/dev/cu.Ultra", APPLICATION_USB_ID, Some("AABBCC001122"));
    let mut scans = [
        vec![bootloader.clone(), unrelated.clone()],
        vec![],
        vec![bootloader, unrelated, application.clone()],
    ]
    .into_iter();
    let found = wait_port(
        "AABBCC001122",
        APPLICATION_USB_ID,
        &cancelled,
        Duration::from_secs(1),
        || Ok(scans.next().expect("Unexpected enumeration retry")),
    )
    .unwrap()
    .unwrap();
    assert_eq!(found.port_name, application.port_name);
    assert!(scans.next().is_none());
}

#[test]
fn startup_wait_times_out_or_cancels_without_accepting_a_bootloader() {
    let cancelled = AtomicBool::new(false);
    let found = wait_port(
        "AABBCC001122",
        APPLICATION_USB_ID,
        &cancelled,
        Duration::ZERO,
        || {
            Ok(vec![port(
                "/dev/cu.Dfu",
                BOOTLOADER_USB_ID,
                Some("AABBCC001122"),
            )])
        },
    )
    .unwrap();
    assert!(found.is_none());

    let error = wait_port(
        "AABBCC001122",
        APPLICATION_USB_ID,
        &cancelled,
        Duration::from_secs(1),
        || {
            cancelled.store(true, Ordering::Relaxed);
            Ok(vec![])
        },
    )
    .unwrap_err();
    assert!(error.is::<RequestCancelled>());
}

#[cfg(target_os = "macos")]
#[test]
fn automatic_discovery_uses_callout_ports_but_accepts_an_explicit_tty() {
    let tty = port("/dev/tty.Ultra", BOOTLOADER_USB_ID, Some("AABBCC001122"));
    let callout = port("/dev/cu.Ultra", BOOTLOADER_USB_ID, Some("AABBCC001122"));
    assert_eq!(
        select(vec![tty.clone(), callout.clone()], None)
            .unwrap()
            .port_name,
        callout.port_name
    );
    assert_eq!(
        select(vec![tty.clone(), callout], Some("/dev/tty.Ultra"))
            .unwrap()
            .port_name,
        tty.port_name
    );
}
