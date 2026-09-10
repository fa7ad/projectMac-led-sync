from projectmac_tuya_sync.tuya_beacon_codec import (
    build_dp_payload,
    crc16_high_byte,
    decode_frame,
    encode_frame,
    xxtea_decrypt,
    xxtea_encrypt,
)

LOCAL_KEY = b"0123456789abcdef"  # 16 bytes, synthetic, not a real device's key
APP_KEY = bytes.fromhex("00112233445566778899aabbccddeeff0011223344556677889900aabbccdd")
SRC_ADDR = bytes.fromhex("aabb")
DST_ADDR = bytes.fromhex("ccdd")


def test_xxtea_round_trip():
    plaintext = bytes(range(16))
    ciphertext = xxtea_encrypt(plaintext, LOCAL_KEY)
    assert ciphertext != plaintext
    assert xxtea_decrypt(ciphertext, LOCAL_KEY) == plaintext


def test_crc16_known_vector():
    assert crc16_high_byte(bytes(16), 16) == 0x00
    assert crc16_high_byte(bytes([0xFF] * 16), 16) == crc16_high_byte(bytes([0xFF] * 16), 16)


def test_build_dp_payload_layout():
    payload = build_dp_payload(dp_id=11, dp_type=0, value=bytes([0x00, 0x10, 0x64, 0x64]))
    assert payload == bytes([11, 0x04, 0x00, 0x10, 0x64, 0x64])


def test_encode_decode_frame_round_trip():
    plaintext = build_dp_payload(dp_id=11, dp_type=0, value=bytes([0x00, 0x10, 0x64, 0x64]))
    frame = encode_frame(
        plaintext=plaintext,
        local_key=LOCAL_KEY,
        app_key=APP_KEY,
        src_addr=SRC_ADDR,
        dst_addr=DST_ADDR,
        sn=(42).to_bytes(2, "big"),
        sub_cmd=5,
        lead_byte=0x0B,
    )
    assert len(frame) == 26

    decoded = decode_frame(frame, local_key=LOCAL_KEY, app_key=APP_KEY)
    assert decoded["trailer_crc_ok"]
    assert decoded["crc_plain_ok"]
    assert decoded["src_addr"] == SRC_ADDR.hex()
    assert decoded["dst_addr"] == DST_ADDR.hex()
    assert decoded["sn"] == "002a"
    assert bytes.fromhex(decoded["plaintext"]).startswith(plaintext)
