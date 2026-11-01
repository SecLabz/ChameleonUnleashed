//! Serial device fixtures shared by protocol and slot tests.
use crate::{Client, command::*};
use serialport::{ClearBuffer, DataBits, FlowControl, Parity, SerialPort, StopBits};
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::{
    sync::{Arc, atomic::AtomicBool},
    time::Duration,
};

struct Port(UnixStream);
impl Read for Port {
    fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
        self.0.read(bytes)
    }
}
impl Write for Port {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.0.write(bytes)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.0.flush()
    }
}
impl SerialPort for Port {
    fn name(&self) -> Option<String> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn baud_rate(&self) -> serialport::Result<u32> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn data_bits(&self) -> serialport::Result<DataBits> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn flow_control(&self) -> serialport::Result<FlowControl> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn parity(&self) -> serialport::Result<Parity> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn stop_bits(&self) -> serialport::Result<StopBits> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn timeout(&self) -> Duration {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_baud_rate(&mut self, _baud_rate: u32) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_data_bits(&mut self, _data_bits: DataBits) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_flow_control(&mut self, _flow_control: FlowControl) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_parity(&mut self, _parity: Parity) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_stop_bits(&mut self, _stop_bits: StopBits) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_timeout(&mut self, _timeout: Duration) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn write_request_to_send(&mut self, _level: bool) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn write_data_terminal_ready(&mut self, _level: bool) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn read_clear_to_send(&mut self) -> serialport::Result<bool> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn read_data_set_ready(&mut self) -> serialport::Result<bool> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn read_ring_indicator(&mut self) -> serialport::Result<bool> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn read_carrier_detect(&mut self) -> serialport::Result<bool> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn bytes_to_read(&self) -> serialport::Result<u32> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn bytes_to_write(&self) -> serialport::Result<u32> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn clear(&self, _buffer_to_clear: ClearBuffer) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn try_clone(&self) -> serialport::Result<Box<dyn SerialPort>> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn set_break(&self) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
    fn clear_break(&self) -> serialport::Result<()> {
        unreachable!("Serial configuration is outside this protocol test")
    }
}

type Exchange = (u16, Vec<u8>, u16, Vec<u8>);

pub(crate) fn device(exchanges: Vec<Exchange>) -> (Client, std::thread::JoinHandle<()>) {
    device_with_hook(exchanges, |_| {})
}

pub(crate) fn device_with_hook(
    exchanges: Vec<Exchange>,
    mut hook: impl FnMut(u16) + Send + 'static,
) -> (Client, std::thread::JoinHandle<()>) {
    let (host, mut device) = UnixStream::pair().unwrap();
    host.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    device
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    let worker = std::thread::spawn(move || {
        for (command, payload, status, reply) in exchanges {
            let expected = crate::frame::encode(command, 0, &payload).unwrap();
            let mut received = vec![0; expected.len()];
            device
                .read_exact(&mut received)
                .unwrap_or_else(|e| panic!("command {command}, {payload:?}: {e}"));
            assert_eq!(received, expected);
            hook(command);
            device
                .write_all(&crate::frame::encode(command, status, &reply).unwrap())
                .unwrap();
        }
        // Keep the final reply readable and reject unexpected cleanup retries.
        assert_eq!(device.read(&mut [0]).unwrap(), 0);
    });
    (
        Client {
            port: Box::new(Port(host)),
            usable: true,
            cancelled: Arc::new(AtomicBool::new(false)),
            capabilities: [COLLECT_OBSERVATIONS, CHECK_KEYS, READ_BLOCK].into(),
        },
        worker,
    )
}

pub(crate) fn device_disconnect(
    exchanges: Vec<Exchange>,
    dropped_command: u16,
    dropped_payload: Vec<u8>,
) -> (Client, std::thread::JoinHandle<()>) {
    let (host, mut device) = UnixStream::pair().unwrap();
    host.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    device
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    let worker = std::thread::spawn(move || {
        for (command, payload, status, reply) in exchanges {
            let expected = crate::frame::encode(command, 0, &payload).unwrap();
            let mut received = vec![0; expected.len()];
            device.read_exact(&mut received).unwrap();
            assert_eq!(received, expected);
            device
                .write_all(&crate::frame::encode(command, status, &reply).unwrap())
                .unwrap();
        }
        let expected = crate::frame::encode(dropped_command, 0, &dropped_payload).unwrap();
        let mut received = vec![0; expected.len()];
        device.read_exact(&mut received).unwrap();
        assert_eq!(received, expected);
    });
    (
        Client {
            port: Box::new(Port(host)),
            usable: true,
            cancelled: Arc::new(AtomicBool::new(false)),
            capabilities: [dropped_command].into(),
        },
        worker,
    )
}
