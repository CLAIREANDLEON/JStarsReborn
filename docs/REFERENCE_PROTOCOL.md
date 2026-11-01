# PS4 reference measurement protocol

Status: defined, not executed. No gameplay captures or reference measurements have been supplied.

## Capture record

For every capture, record a unique evidence ID, title/version/region/build, hardware, display mode, capture-device settings, capture frame rate, original timestamp units, dropped-frame checks, controller connection, input sequence, match/mode setup and any uncontrolled factors. Keep a SHA-256 of the original capture and its authorized-use/provenance note. Preserve originals. Do not infer simulation rate from a video's nominal frame rate.

Use this machine-readable trial shape when actual data is available. Null values below mean unmeasured; they are not zero or defaults.

```json
{
  "schema": "reborn.reference-trial.v1",
  "trial_id": "replace-with-evidence-id",
  "reference_build": null,
  "capture_sha256": null,
  "capture_fps_numerator": null,
  "capture_fps_denominator": null,
  "scenario": null,
  "initial_conditions": {},
  "inputs": [],
  "observations": [],
  "uncertainty_notes": [],
  "review_status": "unmeasured"
}
```

An input entry includes action, pressed/released state, controller index, timestamp and clock source. An observation includes event type, timestamp or frame index, measured value, units, uncertainty interval and evidence location. Never invent unseen hitboxes, animation states, internal RNG seeds or damage formulas from visual resemblance.

## Repeatable trials

1. Record idle, locomotion, turns, dash, jump and camera response before combat trials. Establish coordinate, speed and timing conventions from evidence.
2. For one verified character pairing and stage, repeat isolated actions from controlled initial conditions. Measure input-to-visible-response, start-up, active/recovery windows where observable, damage, hit stun, knockback and cancellation conditions.
3. Add combos, transformations, specials, team interactions and environmental interactions only after single-action measurements are reproducible. Keep uncertainty and conflicting observations.
4. Measure AI and mode progression through separate scenarios. Random outcomes require multiple trials; no sample count or tolerance is assumed sufficient in advance.
5. Run the identical independently specified inputs on PC. Compare event timelines and numerical observables against agreed uncertainty bounds. Rendering may interpolate, but simulation event timing must not accelerate at 30/60/120/unlocked render rates.

## Comparison and sign-off

A scenario passes only with reference evidence, reproducible PC input/state logs, explicit tolerances and reviewed deviations. Record tested and failing trials without deleting failures. A screenshot that looks similar is insufficient evidence for timing, hit registration or AI equivalence.

Before choosing a network model, replay identical input streams repeatedly and compare per-tick canonical state hashes on supported builds and hardware. Measure divergence, state size, resimulation cost and behavior under latency/jitter/loss. No rollback, server-authoritative, crossplay or online stability claim follows from the present runtime foundation.
