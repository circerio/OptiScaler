# Compact billboard/marker evidence

This file preserves conclusions and reproducible controls without committing large raw captures.

## Fixtures

- Original draw: shader `0x499D48C2`, dialogue/shop white markers in the previously used town scene.
- Passive control: `SoraUIExperimentMode=10`; original pre-C9 draw remains untouched.
- Frozen snapshot: F10 captures marker-only RGBA16F; velocity is controlled by `SoraMarkerVelocityPixelsPerFrame`.
- 4X is the primary discriminator; 2X is regression/reference.
- Modes 9/11/12 provide generated-only, BiasCurrentColorHint and recomposition A/B controls respectively.

## Passive forensics

1,058 application frames / 4,232 marker records had exact application-frame mapping and no staging drops. Only the first 64 bytes of the 192-byte instance payload changed. Atlas/UV, bytes 64–191, tint/alpha, depth-fade constants, camera and bound depth/UI resources stayed invariant. Projected positions and corners were subpixel-continuous; allocator offset and instance reorder events did not coincide with worse morphology.

For source moves >=25 px/application frame, endpoint-like frames had median bright-area proxy 4,687 and edge-area 200; interior/generated-like frames had 4,408 and 279. A short sequence showed malformed intermediate positions followed by clean next endpoints while source transforms remained smooth.

## Frozen velocity sweep

Completed speeds: 2, 10, 25, 32 and 40 px/application frame. Endpoints were clean at every speed. Generated quarter/half phases acquired thin irregular contamination outside the marker edge; severity broadly increased with distance from a clean endpoint. There is no defensible hard onset threshold.

Adjusted ring20 examples: 10 px quarter/half 0.520%/1.008%; 25 px 1.740%/10.085%; 32 px 2.981%/4.131%; 40 px 3.961%/28.269%. These are representative sampled intervals, not a monotonic performance curve.

## Falsified or weakened hypotheses

- One-frame Final/HUD-less/UI misalignment: fixed and then rejected by 18,850 matching token groups.
- Large HDR/composition-equation mismatch: rejected within R10 quantization tolerance.
- Generic inability to interpolate moving UI: rejected by clean linear synthetic UI.
- UIAlpha reconstruction as the main cause: RGBA did not eliminate the original problem.
- UI recomposition as a necessary cause: strict mode-12 OFF and ON both reproduced the same phase-dependent morphology.
- BiasCurrentColorHint as a sufficient fix: rejected by mode 11.
- Transform snapping, repeated-then-jump, atlas/UV/identity/appearance switches: rejected in the passive interval.

## Current causal boundary

The application endpoints are clean. The corruption is associated with generated/intermediate phases of fast overlay motion. It exists with UI recomposition both off and on, so the remaining boundary is common DLSSG synthesis or later shared HDR/composition/presentation. A unique MV/depth cause is not proven.

The next single high-information diagnostic is to capture/read back the generated surface immediately after DLSSG and before the common HDR/presenter boundary. Do not infer a correction until that test locates the contamination.

White-marker mode 2 is a useful 2X playable improvement, not a universal repair. Black billboards are not handled.
