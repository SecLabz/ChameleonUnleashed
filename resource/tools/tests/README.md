# Regression fixtures

`desfire-public.json` is a synthetic DESFire image shared by native, Rust and
browser tests. Its UID, batch, application ID and contents use a fixed public
seed. Regenerate or verify it from the repository root:

```sh
python3 resource/tools/generate_desfire_fixture.py
python3 resource/tools/generate_desfire_fixture.py --check
```

Crypto1 vector inputs in `software/script/tests/test_crypto1.py` use
`random.Random(0xC1F17E57)`: draw the shared 32-bit UID, then a 48-bit key,
32-bit tag nonce and 32-bit reader nonce for each vector. Expected replies were
evaluated with `software/src/crypto1.c`, independently of the Python code under
test. Darkside tests share synthetic nonce and mock frame values across C,
Rust and JavaScript.

## RF recordings

`st25tb/captures.zip` contains 54 original ADC-bank recordings used by
`test_st25tb_receiver.py` and `test_st25tb_pm3.py`. Compression is lossless;
the archive retains the signal defects needed by receiver regressions.
Extract individual files for the replay tool with:

```sh
python3 -m zipfile -e resource/tools/tests/st25tb/captures.zip /tmp/st25tb-captures
resource/tools/st25tb/replay.sh /tmp/st25tb-captures/lost/m1.bin
```

`st25tb/pm3-trace.txt` tests read coverage, preserving the transcript layout,
exchange timing and missing reply with synthetic UID and memory values and
matching CRC-B replies. Its UART path is a generic example. `st25tb/slot.json`
supplies the matching synthetic emulator image for the PM3 rig and its tests:
wire-order UID `01 02 03 04 05 06 02 D0`, memory bytes `00` through `3F`,
system block `FF FF FF FF` and selection byte `A1`.

The ADC recordings in `st25tb/captures.zip` retain physical tag data and have
not been anonymized. Replacing decoded text does not anonymize those signals.

The five `t55xx_*_capture.bin.gz` files are physical LF waveforms consumed by
`test_lf_protocols.py`. They exercise envelope drift, NRZ data, PSK phase
excursions and long runs of identical bits. Changing their payloads requires
equivalent waveform regressions; randomizing ADC bytes destroys those cases.
