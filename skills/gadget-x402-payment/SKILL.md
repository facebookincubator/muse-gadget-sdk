---
name: gadget-x402-payment
description: >-
  Let a Muse gadget pay over x402: fetch a 402 PaymentRequirements response,
  sign a gasless EIP-712 payment authorization with the gadget's wallet, and
  retry with the X-PAYMENT header. Use when the gadget (or the agent driving it)
  needs to spend — buying API access, tipping, or paying its own way — with
  on-device confirmation and hard spend caps.
---

# x402 Payments from a Gadget

Meta's gadget kit gives a board eyes, ears, voice, and a screen. This skill
gives it a wallet. Any x402 seller on the internet becomes payable from a
$15 board: the gadget reads the machine-readable terms, the user confirms on
the display, and the payment settles as USDC on Base.

Use this skill when the gadget needs to spend money on the user's behalf, or
when a device needs to pay for its own API/data access autonomously.

## Identify the Seller

- GET the seller's `/.well-known/x402` and confirm it returns machine-readable
  payment terms (price, accepted assets, accepted networks, pay-to address).
- A normal request to the paid resource returns **HTTP 402** with a
  `PaymentRequirements` body. Confirm the response carries an `accepts[]`
  array (multi-rail shape) — never assume a single rail.
- Keep wallet addresses and payment payloads private; never log a signed
  authorization.

## Prerequisites

- The gadget host runs the reference client (`gadget_pay.py`) or equivalent:
  Python 3.10+, `requests`, `eth-account`.
- A gadget wallet exists (local encrypted keystore). **Testnet default.**
  Mainnet requires the explicit mainnet confirmation string — see Limits.
- The gadget has a display (or speaker) capable of showing the payment
  summary for confirmation. No display, no payments over the confirm
  threshold.

## Workflow

1. **Discover.** GET `/.well-known/x402` on the seller. Read price, asset,
   network, and pay-to address. Present the plain-language summary on the
   gadget display: what, how much, to whom.
2. **Quote.** GET the paid resource without payment. Expect 402 +
   `PaymentRequirements`. Match one entry of `accepts[]` against the rails
   the gadget wallet funds (Base USDC first).
3. **Authorize (under cap).** If the amount is at or under the auto-approve
   threshold (default $1.00), the gadget may sign immediately with its
   EIP-712 gasless authorization. Record the spend against the lifetime cap.
4. **Confirm (over cap).** If the amount exceeds the auto-approve threshold,
   show amount + recipient + what-it-buys on the display and wait for the
   physical confirm (button press / touch). No confirm, no signature. Ever.
5. **Pay.** Retry the request with the `X-PAYMENT` header carrying the signed
   authorization. Expect 200 and a settlement receipt.
6. **Receipt.** Display the receipt on screen: amount, asset, network,
   transaction hash, timestamp. Speak a one-line confirmation if the gadget
   has a speaker.

## Verify the Result

- The 200 response must include a settlement receipt; read back the tx hash
  and confirm it on-chain (or via the facilitator) before reporting success.
- The wallet's lifetime-spent counter must equal previous spent + this amount,
  to the cent. Reconcile after every payment.
- A 402 on the retry means the payment was rejected — surface the reason,
  do not re-sign blindly.

## Limits

- **Hard caps.** Per-call cap and lifetime cap are enforced in the wallet,
  not in the UI. Default lifetime cap $25. Caps only move with the user's
  explicit instruction.
- **Testnet default.** Mainnet payments require the exact mainnet
  confirmation string typed by the user — a gadget button cannot supply it.
- **No silent spending.** Anything over the auto-approve threshold needs
  physical confirmation on the device. No API, no agent, no skill can waive
  this.
- **No custodial keys.** The gadget holds its own key in a local encrypted
  keystore. This skill never sends a private key anywhere.
- This skill does not implement refunds, subscriptions, or recurring charges.

## Sources

- [x402 protocol](https://www.x402.org/) — HTTP 402 payment standard
- [AwLPay](https://github.com/ceedot-rock/awlpay) — multi-rail x402 facilitator
  and SDK used by the reference client (Apache-2.0)
