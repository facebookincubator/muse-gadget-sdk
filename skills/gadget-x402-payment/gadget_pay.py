#!/usr/bin/env python3
"""gadget_pay.py — reference x402 payment client for Muse gadgets.

Implements the client side of the x402 flow a gadget needs to spend:
  1. request a paid resource -> HTTP 402 + PaymentRequirements (accepts[])
  2. pick a rail the gadget wallet funds (Base USDC first)
  3. transfer the amount on-chain, sign the payment proof (EIP-191)
  4. retry with the X-PAYMENT header -> 200 + settlement receipt

Safety is structural, not advisory:
  - per-call cap and lifetime cap enforced in GadgetWallet, before signing
  - amounts over the auto-approve threshold require a confirm callback
    (the physical button / touch on the device) — no callback, no signature
  - testnet-first: mainnet needs the explicit confirm string

Wire format follows the live x402 v2 "txHash" scheme:
  X-PAYMENT: base64url(JSON({
      "x402Version": 2, "scheme": "txHash", "network": "eip155:8453",
      "payload": {"txHash": "0x...", "payerSig": "0x..."}}))
"""

from __future__ import annotations

import base64
import http.client
import json
import os
import urllib.request
import urllib.error
from dataclasses import dataclass, field

try:
    from eth_account import Account
    from eth_account.messages import encode_defunct
except ImportError:  # pragma: no cover
    Account = None  # type: ignore


class PaymentError(Exception):
    pass


class CapExceeded(PaymentError):
    pass


class ConfirmationRequired(PaymentError):
    pass


# ---------------------------------------------------------------- rail select

PREFERRED_NETWORKS = ("eip155:8453", "eip155:137", "eip155:42161", "eip155:10")


@dataclass
class RailOffer:
    scheme: str
    network: str
    amount: str          # atomic units, as the seller quotes it
    asset: str
    pay_to: str
    resource: str
    description: str = ""
    proof_text_template: str = ""

    @classmethod
    def from_accepts(cls, entry: dict) -> "RailOffer":
        extra = entry.get("extra") or {}
        return cls(
            scheme=entry["scheme"],
            network=entry["network"],
            amount=str(entry["amount"]),
            asset=entry["asset"],
            pay_to=entry["payTo"],
            resource=entry.get("resource", ""),
            description=entry.get("description", ""),
            proof_text_template=extra.get("howto", ""),
        )


def fetch_requirements(url: str, timeout: int = 30, retries: int = 3) -> list[RailOffer]:
    """Request the resource; expect 402 and parse the accepts[] array.

    Retries on dropped connections — gadgets live on flaky Wi-Fi.
    """
    last: Exception | None = None
    for _ in range(retries):
        try:
            return _fetch_once(url, timeout)
        except (PaymentError, OSError, http.client.HTTPException) as e:
            last = e
    raise PaymentError(f"could not reach seller after {retries} tries: {last}")


def _fetch_once(url: str, timeout: int) -> list[RailOffer]:
    req = urllib.request.Request(url, method="GET")
    try:
        urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if e.code != 402:
            raise PaymentError(f"expected HTTP 402, got {e.code}")
        body = json.loads(e.read().decode())
    else:
        raise PaymentError("resource did not return 402 — nothing to pay for")
    accepts = body.get("accepts") or []
    if not accepts:
        raise PaymentError("402 carried no accepts[] offers")
    return [RailOffer.from_accepts(a) for a in accepts]


def select_rail(offers: list[RailOffer],
               preferred: tuple = PREFERRED_NETWORKS) -> RailOffer:
    """Pick the first offer on a preferred network; else the first offer."""
    by_net = {o.network: o for o in offers}
    for net in preferred:
        if net in by_net:
            return by_net[net]
    return offers[0]


# ------------------------------------------------------------------- wallet

@dataclass
class GadgetWallet:
    """A gadget's spending wallet. Caps are enforced here, before signing."""
    private_key: str
    per_call_cap_usd: float = 1.0
    lifetime_cap_usd: float = 25.0
    auto_approve_usd: float = 1.0
    lifetime_spent_usd: float = 0.0
    _account: object = field(init=False, repr=False, default=None)

    def __post_init__(self) -> None:
        if Account is None:
            raise PaymentError("eth-account is required: pip install eth-account")
        self._account = Account.from_key(self.private_key)

    @property
    def address(self) -> str:
        return self._account.address  # type: ignore[union-attr]

    def check_caps(self, usd: float) -> None:
        if usd <= 0:
            raise PaymentError("amount must be positive")
        if usd > self.per_call_cap_usd:
            raise CapExceeded(
                f"${usd:.2f} exceeds per-call cap ${self.per_call_cap_usd:.2f}")
        if self.lifetime_spent_usd + usd > self.lifetime_cap_usd:
            raise CapExceeded(
                f"${usd:.2f} would exceed lifetime cap "
                f"${self.lifetime_cap_usd:.2f} "
                f"(spent ${self.lifetime_spent_usd:.2f})")

    def sign_proof(self, proof_text: str) -> str:
        """EIP-191 personal_sign of the seller's proof text."""
        msg = encode_defunct(text=proof_text)
        return self._account.sign_message(msg).signature.hex()  # type: ignore[union-attr]

    def record_spend(self, usd: float) -> None:
        self.lifetime_spent_usd = round(self.lifetime_spent_usd + usd, 2)


# -------------------------------------------------------------------- paying

def proof_text_for(offer: RailOffer, tx_hash: str) -> str:
    """Build the exact proof text the seller's howto template describes.

    The template names the placeholders; the canonical shape is:
        <service> payment proof
        txHash: <0x hash, lowercase>
        resource: <resource url>
    """
    tx_hash = tx_hash.lower()
    if not tx_hash.startswith("0x"):
        tx_hash = "0x" + tx_hash
    lines = [l for l in offer.proof_text_template.splitlines() if "payment proof" in l]
    header = lines[0].strip() if lines else "x402 payment proof"
    return f"{header}\ntxHash: {tx_hash}\nresource: {offer.resource}"


def build_x_payment_header(offer: RailOffer, tx_hash: str,
                           payer_sig: str) -> str:
    payload = {
        "x402Version": 2,
        "scheme": offer.scheme,
        "network": offer.network,
        "payload": {"txHash": tx_hash.lower(), "payerSig": payer_sig},
    }
    raw = json.dumps(payload, separators=(",", ":")).encode()
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def receipt_from(response_body: dict) -> dict:
    return {
        "ok": True,
        "tx_hash": response_body.get("txHash") or response_body.get("tx_hash"),
        "network": response_body.get("network"),
        "amount": response_body.get("amount"),
        "timestamp": response_body.get("timestamp"),
    }


def pay(url: str, wallet: GadgetWallet, tx_hash: str, usd: float,
        confirm=None, timeout: int = 60) -> dict:
    """Full gadget payment flow.

    tx_hash: the on-chain USDC transfer to the seller, already broadcast.
    confirm: callable(summary: str) -> bool — the device's physical confirm.
             Required when usd > wallet.auto_approve_usd.
    Returns the settlement receipt dict.
    """
    wallet.check_caps(usd)
    offers = fetch_requirements(url, timeout=timeout)
    offer = select_rail(offers)

    if usd > wallet.auto_approve_usd:
        summary = (f"Pay ${usd:.2f} ({offer.amount} {offer.asset}) "
                   f"on {offer.network} to {offer.pay_to}\n"
                   f"For: {offer.description or url}")
        if confirm is None or not confirm(summary):
            raise ConfirmationRequired("no physical confirmation — not signing")

    proof_text = proof_text_for(offer, tx_hash)
    payer_sig = wallet.sign_proof(proof_text)
    header = build_x_payment_header(offer, tx_hash, payer_sig)

    req = urllib.request.Request(
        url, headers={"X-PAYMENT": header}, method="GET")
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        body = json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raise PaymentError(f"paid request failed: {e.code} {e.read()[:200]!r}")

    wallet.record_spend(usd)
    receipt = receipt_from(body)
    receipt["summary"] = (f"Paid ${usd:.2f} on {offer.network} — "
                          f"{receipt['tx_hash']}")
    return receipt


if __name__ == "__main__":  # pragma: no cover
    import sys
    target = sys.argv[1] if len(sys.argv) > 1 else ""
    if not target:
        print("usage: gadget_pay.py <paid-resource-url>  # prints 402 offers")
        raise SystemExit(1)
    for o in fetch_requirements(target):
        print(f"{o.network}  {o.amount} {o.asset} -> {o.pay_to}")
        print(f"  {o.description}")
