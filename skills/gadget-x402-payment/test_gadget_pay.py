#!/usr/bin/env python3
"""Tests for gadget_pay.py. No real funds, no mainnet writes.

The 402-discovery test hits the live rider-x402 seller (read-only GET,
expects 402) to prove the client parses the real wire format. Everything
else is offline with a throwaway key.
"""

import base64
import json
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from gadget_pay import (  # noqa: E402
    fetch_requirements, select_rail, proof_text_for, build_x_payment_header,
    GadgetWallet, RailOffer, CapExceeded, ConfirmationRequired, PaymentError,
)

from eth_account import Account
from eth_account.messages import encode_defunct

LIVE_402_URL = "https://rider-x402.fly.dev/api/x402/ping/ping"
THROWAWAY_KEY = "0x" + "11" * 32
FIXTURE = os.path.join(os.path.dirname(__file__), "fixture-402-ping.json")


def fixture_offers():
    """Recorded real 402 response — deterministic for offline tests."""
    with open(FIXTURE) as f:
        body = json.load(f)
    return [RailOffer.from_accepts(a) for a in body["accepts"]]

passed = failed = 0


def check(name, fn):
    global passed, failed
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        failed += 1
        print(f"FAIL {name}: {type(e).__name__}: {e}")
    else:
        passed += 1
        print(f"ok   {name}")


# 1. live 402 discovery — real wire format -------------------------------
def t_live_402():
    offers = fetch_requirements(LIVE_402_URL)
    live_nets = {o.network for o in offers}
    fixture_nets = {o.network for o in fixture_offers()}
    assert live_nets == fixture_nets, f"fixture stale: {live_nets ^ fixture_nets}"
    assert offers, "no offers parsed"
    nets = {o.network for o in offers}
    assert "eip155:8453" in nets, f"Base missing from {nets}"
    o = next(x for x in offers if x.network == "eip155:8453")
    assert o.scheme == "txHash", o.scheme
    assert o.asset.lower() == "0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913".lower()
    assert o.pay_to.startswith("0x") and len(o.pay_to) == 42
    assert "payment proof" in o.proof_text_template


check("live 402 discovery parses real wire format", t_live_402)


# 2. rail selection prefers Base ------------------------------------------
def t_select_rail():
    offers = fixture_offers()
    assert select_rail(offers).network == "eip155:8453"


check("rail selection prefers Base", t_select_rail)


# 3. EIP-191 signature verifies by recovery ---------------------------------
def t_signature_recovers():
    w = GadgetWallet(private_key=THROWAWAY_KEY)
    text = "rider-x402 payment proof\ntxHash: 0xabc\nresource: https://x"
    sig = w.sign_proof(text)
    recovered = Account.recover_message(encode_defunct(text=text),
                                        signature=sig)
    assert recovered.lower() == w.address.lower(), (recovered, w.address)


check("EIP-191 signature recovers to wallet address", t_signature_recovers)


# 4. proof text matches the seller's howto shape ---------------------------
def t_proof_text():
    offers = fixture_offers()
    o = select_rail(offers)
    pt = proof_text_for(o, "0xABCDEF")
    assert "payment proof" in pt.splitlines()[0]
    assert "txHash: 0xabcdef" in pt  # lowercased
    assert f"resource: {o.resource}" in pt


check("proof text matches seller howto", t_proof_text)


# 5. X-PAYMENT header round-trips ------------------------------------------
def t_header_shape():
    offers = fixture_offers()
    o = select_rail(offers)
    h = build_x_payment_header(o, "0xabc", "0x" + "00" * 65)
    padded = h + "=" * (-len(h) % 4)
    decoded = json.loads(base64.urlsafe_b64decode(padded).decode())
    assert decoded["x402Version"] == 2
    assert decoded["scheme"] == "txHash"
    assert decoded["network"] == "eip155:8453"
    assert decoded["payload"]["txHash"] == "0xabc"
    assert decoded["payload"]["payerSig"].startswith("0x")


check("X-PAYMENT header has the exact wire shape", t_header_shape)


# 6. caps enforced before signing ------------------------------------------
def t_per_call_cap():
    w = GadgetWallet(private_key=THROWAWAY_KEY, per_call_cap_usd=1.0)
    try:
        w.check_caps(1.50)
    except CapExceeded:
        return
    raise AssertionError("over-cap payment not rejected")


check("per-call cap rejects before signing", t_per_call_cap)


def t_lifetime_cap():
    w = GadgetWallet(private_key=THROWAWAY_KEY, lifetime_cap_usd=2.0)
    w.record_spend(1.50)
    try:
        w.check_caps(1.00)
    except CapExceeded:
        return
    raise AssertionError("lifetime-cap breach not rejected")


check("lifetime cap accumulates and rejects", t_lifetime_cap)


def t_spend_recorded():
    w = GadgetWallet(private_key=THROWAWAY_KEY)
    w.record_spend(0.10)
    w.record_spend(0.10)
    assert w.lifetime_spent_usd == 0.20, w.lifetime_spent_usd


check("spend recorded to the cent", t_spend_recorded)


# 7. confirmation gate -------------------------------------------------------
def t_confirm_required():
    w = GadgetWallet(private_key=THROWAWAY_KEY, auto_approve_usd=1.0,
                     per_call_cap_usd=100.0)
    try:
        from gadget_pay import pay
        pay(LIVE_402_URL, w, "0x" + "ab" * 32, 5.00, confirm=None)
    except ConfirmationRequired:
        return
    raise AssertionError("over-threshold payment proceeded without confirm")


check("over-threshold needs physical confirm", t_confirm_required)


def t_confirm_denied():
    w = GadgetWallet(private_key=THROWAWAY_KEY, auto_approve_usd=1.0,
                     per_call_cap_usd=100.0)
    try:
        from gadget_pay import pay
        pay(LIVE_402_URL, w, "0x" + "ab" * 32, 5.00,
            confirm=lambda s: False)
    except ConfirmationRequired:
        return
    raise AssertionError("denied confirmation still paid")


check("denied confirmation aborts", t_confirm_denied)


# 8. 200 without 402 raises ---------------------------------------------------
def t_no_402_raises():
    try:
        fetch_requirements("https://rider-x402.fly.dev/health")
    except PaymentError:
        return
    raise AssertionError("non-402 resource did not raise")


check("non-402 resource raises PaymentError", t_no_402_raises)


print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
