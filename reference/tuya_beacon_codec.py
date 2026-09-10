"""Tuya BLE Beacon Protocol frame codec — reverse-engineered from Smart Life
classes14.dex (com.thingclips.sdk.bluetooth.pbddddb / ddbqqbd / bbdqqbd).

Copied unmodified from the original research repo except for one thing: the
original file's `_self_test()` re-encoded a real captured frame from one
specific device's real pairing (embedding that device's real LOCAL_KEY/
APP_KEY plus a real captured frame's bytes) to prove `encode_frame` byte-for-
byte correct — the strongest possible check, but not something that can be
committed to a public repo, since the fixture itself is derived from real
secret key material. That test (and its embedded secrets) has been removed
here. See PLAN.md's "Secrets handling" section for what to write instead: a
synthetic round-trip test using arbitrary non-secret keys, which proves the
algorithm without needing anyone's real pairing data.

Every function below is otherwise untouched and takes key material as plain
parameters — nothing here is specific to any one device/account.
"""

DELTA = 0x9E3779B9
MASK = 0xFFFFFFFF


def _bytes_to_words_be(b):
    n = (len(b) + 3) // 4
    words = [0] * n
    for i, byte in enumerate(b):
        words[i >> 2] |= byte << ((3 - (i & 3)) * 8)
    return words


def _words_to_bytes_be(words, length):
    out = bytearray(length)
    for i in range(length):
        out[i] = (words[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF
    return bytes(out)


def xxtea_decrypt(data: bytes, key: bytes) -> bytes:
    v = _bytes_to_words_be(data)
    k = _bytes_to_words_be(key)
    n = len(v)
    rounds = 6 + 52 // n
    total = (rounds * DELTA) & MASK
    y = v[0]
    while total != 0:
        e = (total >> 2) & 3
        for p in range(n - 1, 0, -1):
            z = v[p - 1]
            mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) & MASK
            mx ^= ((total ^ y) + (k[(p & 3) ^ e] ^ z)) & MASK
            v[p] = (v[p] - mx) & MASK
            y = v[p]
        z = v[n - 1]
        mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) & MASK
        mx ^= ((total ^ y) + (k[e] ^ z)) & MASK
        v[0] = (v[0] - mx) & MASK
        y = v[0]
        total = (total - DELTA) & MASK
    return _words_to_bytes_be(v, len(data))


def xxtea_encrypt(data: bytes, key: bytes) -> bytes:
    v = _bytes_to_words_be(data)
    k = _bytes_to_words_be(key)
    n = len(v)
    rounds = 6 + 52 // n
    total = 0
    z = v[n - 1]
    for _ in range(rounds):
        total = (total + DELTA) & MASK
        e = (total >> 2) & 3
        for p in range(0, n - 1):
            y = v[p + 1]
            mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) & MASK
            mx ^= ((total ^ y) + (k[(p & 3) ^ e] ^ z)) & MASK
            v[p] = (v[p] + mx) & MASK
            z = v[p]
        y = v[0]
        p = n - 1
        mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) & MASK
        mx ^= ((total ^ y) + (k[(p & 3) ^ e] ^ z)) & MASK
        v[n - 1] = (v[n - 1] + mx) & MASK
        z = v[n - 1]
    return _words_to_bytes_be(v, len(data))


def crc16_high_byte(data: bytes, count: int, poly=33664) -> int:
    """bbdqqbd.bdpdqbp(byte[], int) — CRC16-like, returns high byte."""
    s = 0
    for i in range(count):
        s ^= (data[i] << 8) & 0xFFFF
        for _ in range(8):
            if s & 0x8000:
                s ^= poly
            s = (s << 1) & 0xFFFF
    return (s >> 8) & 0xFF


def xor_bytes(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def decode_frame(final_frame: bytes, local_key: bytes, app_key: bytes):
    assert len(final_frame) == 26, f"expected 26 bytes, got {len(final_frame)}"
    lead_byte = final_frame[0]
    inner = final_frame[1:25]
    trailer_crc = final_frame[25]

    header8 = inner[0:8]
    ciphertext_xored = inner[8:24]
    src_addr = header8[0:2]
    dst_addr = header8[2:4]
    sn = header8[4:6]
    sub_cmd = header8[6]
    crc_plain_expected = header8[7]

    ciphertext = xor_bytes(ciphertext_xored, app_key)
    plaintext = xxtea_decrypt(ciphertext, local_key)

    # trailer CRC is computed BEFORE the appKey XOR mutates the inner frame in place
    check = bytes([lead_byte & 0xFC]) + header8 + ciphertext
    computed_trailer = crc16_high_byte(check, 25)
    crc_plain_computed = crc16_high_byte(plaintext, 16)

    return {
        "lead_byte": lead_byte,
        "trailer_crc_given": trailer_crc,
        "trailer_crc_computed": computed_trailer,
        "trailer_crc_ok": trailer_crc == computed_trailer,
        "src_addr": src_addr.hex(),
        "dst_addr": dst_addr.hex(),
        "sn": sn.hex(),
        "sub_cmd": sub_cmd,
        "crc_plain_expected": crc_plain_expected,
        "crc_plain_computed": crc_plain_computed,
        "crc_plain_ok": crc_plain_expected == crc_plain_computed,
        "plaintext": plaintext.hex(),
    }


def build_dp_payload(dp_id: int, dp_type: int, value: bytes) -> bytes:
    """bppbpqq.bdpdqbp(DpsCombine) — [dpId(1)][type<<4|len(1)][value(len bytes)]."""
    assert 0 <= len(value) <= 15
    return bytes([dp_id, (dp_type << 4) | len(value)]) + value


def encode_frame(
    plaintext: bytes,
    local_key: bytes,
    app_key: bytes,
    src_addr: bytes,
    dst_addr: bytes,
    sn: bytes,
    sub_cmd: int,
    lead_byte: int,
) -> bytes:
    """Mirror of pbddddb.qqdbbpp() + pbddddb.pbbppqb()."""
    assert len(plaintext) <= 16
    assert len(src_addr) == 2 and len(dst_addr) == 2 and len(sn) == 2
    plaintext16 = plaintext + bytes(16 - len(plaintext))
    crc_plain = crc16_high_byte(plaintext16, 16)

    header8 = src_addr + dst_addr + sn + bytes([sub_cmd, crc_plain])
    ciphertext = xxtea_encrypt(plaintext16, local_key)

    # trailer CRC computed over the PRE-xor inner frame, then XOR mutates it after
    check = bytes([lead_byte & 0xFC]) + header8 + ciphertext
    trailer_crc = crc16_high_byte(check, 25)

    ciphertext_xored = xor_bytes(ciphertext, app_key)
    inner = header8 + ciphertext_xored
    return bytes([lead_byte]) + inner + bytes([trailer_crc])


# NOTE: the original file's `_self_test()` (real-frame regression check) and
# `__main__` demo block (which also embedded real key material) were removed
# here — see the module docstring above. Write a synthetic-keys version of
# both before relying on this file.
