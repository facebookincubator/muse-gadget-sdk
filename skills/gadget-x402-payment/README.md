# gadget-x402-payment

The missing product in the Muse Gadgets kit: **a wallet**.

This kit gives a board eyes, ears, voice, and a screen — 43 device skills and
zero commerce. This skill adds x402 payments, so any gadget can spend: buying
API access, tipping, or paying its own way, with on-device confirmation and
hard spend caps.

## What's here

- `SKILL.md` — the skill in this repo's format (frontmatter + Identify /
  Prerequisites / Workflow / Verify / Limits / Sources)
- `gadget_pay.py` — reference Python client for the x402 v2 `txHash` flow:
  402 discovery → rail select → EIP-191 proof signature → `X-PAYMENT` header
- `test_gadget_pay.py` — 11 tests, all green. One hits a live x402 seller
  (read-only, expects 402); the rest are offline with a throwaway key and a
  recorded fixture (`fixture-402-ping.json`)
- `fixture-402-ping.json` — recorded real 402 response for deterministic tests

## The flow

```
gadget -> GET paid resource -> 402 + accepts[] (Base/Polygon/Arbitrum/...)
gadget -> picks rail, shows price + recipient on display
user   -> confirms on the device (over the auto-approve threshold)
gadget -> transfers USDC on-chain, signs EIP-191 proof, retries with X-PAYMENT
seller -> 200 + settlement receipt, shown on screen
```

## Safety

- Per-call cap + lifetime cap enforced in the wallet, before signing
- Over-threshold amounts need physical confirmation on the device — no API
  can waive this
- Testnet-first; the gadget holds its own key in a local encrypted keystore

Built against the live x402 v2 wire format. Reference facilitator/SDK:
[AwLPay](https://github.com/ceedot-rock/awlpay) (Apache-2.0).
