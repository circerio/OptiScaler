# Falcom Engine+ performance attribution

## Controlled result

Test domain: 3840x2160, DLAA, V-Sync off, effectively uncapped, FG off, same `save005` Zeiss scene, ReShade + clean RenoDX held constant.

| Configuration | FPS | Frame time | P95 | GPU busy | CPU time | GPU utilization | GPU clock | GPU power | VRAM |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Falcom Engine+ OFF | 80.75 | 12.384 ms | 13.116 ms | 12.166 ms | 3.252 ms | 97.2% | 3097.2 MHz | 279.1 W | 10043.5 MiB |
| Falcom Engine+ ON | 57.59 | 17.363 ms | 18.378 ms | 16.858 ms | 3.575 ms | 96.2% | 3075 MHz | 294.1 W | 10531 MiB |

Delta: -23.16 FPS, +4.979 ms frame time, +4.692 ms GPU busy, +0.323 ms CPU and +487.5 MiB VRAM. The former “polluted” result was 57.25 FPS / 17.467 ms, only 0.34 FPS / 0.104 ms from FE+ ON. FE+ reproduced approximately 97.9% of the earlier step-down.

## Engineering conclusion

Falcom Engine+ active GPU work explains the observed ~57 FPS performance domain. Persistent FG/OptiScaler/RenoDX pollution was not observed after removal. No production performance fix from the separate forensics branch is included here, and its diagnostic instrumentation must not be merged.

Every future comparison must explicitly record `Falcom Engine+ OFF` or `Falcom Engine+ ON`. Cross-domain FPS comparisons are invalid. Historic 78/80+ to 59 FPS observations may reflect an effective FE+ activation transition, but that history is not promoted to a stronger root-cause claim.
