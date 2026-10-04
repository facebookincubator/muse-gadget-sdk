"""Generate deterministic Noise/envelope/BLE vectors from the Python reference."""
import json, sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import x25519
from musegadget.noise import noise_xx, framing
from musegadget.noise.envelope import (ApplicationResponse, BodyChunk, ServiceFrame)
from musegadget.noise.transport import NoiseTransport, encode_response_envelope
from musegadget.noise.framing import encode_noise_frames
from musegadget.link_client import encode_message, DeviceDescription
from musegadget.ble_framing import encode_chunks

def kp(seed):
    priv = x25519.X25519PrivateKey.from_private_bytes(bytes([seed]) * 32)
    pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return noise_xx._X25519KeyPair(private_key=priv, public_key_bytes=pub)

# Generation order: initiator e, responder e, responder s, initiator s.
seeds = {"init_e": 0x11, "resp_e": 0x22, "resp_s": 0x33, "init_s": 0x44}
queue = [kp(seeds[k]) for k in ("init_e", "resp_e", "resp_s", "init_s")]
noise_xx._generate_x25519_key_pair = lambda: queue.pop(0)
framing._random_int64 = lambda: 0x0123456789ABCDEF

ini, resp = noise_xx.NoiseXXInitiator(), noise_xx.NoiseXXResponder(payload=b"server-hello")
ini.initialize(); resp.initialize()
m1 = ini.write_message1()
m2 = resp.read_message1_and_write_message2(m1)
p2 = ini.read_message2(m2)
m3 = ini.write_message3()
resp.read_message3(m3)
isend, irecv = ini.split()
rsend, rrecv = resp.split()
t = NoiseTransport(isend, irecv)

dev = DeviceDescription(node_id="homelink-abcdef", display_name="iPhone", version="0.1.0",
                        commands={"device.health": {"description": "d", "required": {}, "optional": {}}})
ctrl = t.start_stream_request("POST", "/link-control")
register = {"type": "req", "id": "00000000-0000-0000-0000-000000000001",
            "method": "link.register", "params": dev.register_params()}
body = t.encrypt_body_chunk(ctrl.stream_id, encode_message(register))

# Server -> device: response headers on stream 1, then a link.invoke body chunk.
invoke = {"type": "req", "id": "inv-1", "method": "link.invoke", "command": "device.health", "params": {}}
srv_frames = []
for f in (ServiceFrame.response(1, ApplicationResponse(status=200)),
          ServiceFrame.body_chunk(1, BodyChunk(data=encode_message(invoke)))):
    for pf in encode_noise_frames(encode_response_envelope(f)):
        srv_frames.append(rsend.encrypt_with_ad(b"", pf))
decoded = [t.decrypt_frame(c) for c in srv_frames]
assert decoded[0].kind == "response" and decoded[0].value.status == 200
assert decoded[1].kind == "body_chunk"

out = {
    "seeds": seeds,
    "server_payload": "server-hello",
    "msg1": m1.hex(), "msg2": m2.hex(), "msg3": m3.hex(),
    "handshake_hash": ini.handshake_hash().hex() if False else None,
    "control_request_ciphertexts": [c.hex() for c in ctrl.frames],
    "register_json": json.dumps(register, separators=(",", ":")),
    "register_ciphertexts": [c.hex() for c in body],
    "server_ciphertexts": [c.hex() for c in srv_frames],
    "invoke_json": json.dumps(invoke, separators=(",", ":")),
    "ble_chunks_mtu185": [c.hex() for c in encode_chunks(b'{"type":"device_info","node_id":"homelink-abcdef","pad":"' + b"x" * 300 + b'"}', 185)],
}
json.dump(out, sys.stdout, indent=1)
