# Evidencia de optimización — 19-10-2026

- `manifest.json`: fuentes/binaries SHA256, opciones CMake, plataforma y ejecuciones.
- `artifact_sha256.json`: huellas de todos los archivos de este paquete, salvo sí mismo.
- `validation_summary.json`: métricas recalculadas independientemente desde CSV.
- `optimization_protocol.json`: protocolo congelado antes de medir.
- `baseline_manifest.json`: instantánea previa a los cambios; producción coincide con el commit base.
- `debug/`: 19/19 pruebas de corrección.
- `release/`: 26/26 checks, oráculo real, rankings y protocolo OKR v2.
- `sanitizers_final/`: última suite completa 21/21, incluido el caso real y la CLI; cero diagnósticos.
- `benchmarks/`: muestras de Top-K, memoria, mil consultas agrupadas, mil búsquedas DBpedia y ensamblador.
- `initial-byte-visited/`: primera medición conservada antes de compactar visitados.
- `packed-memory-intermediate/`: observación intermedia RSS de la versión compacta.
- `no-tests/`: build sin pruebas y dos smoke checks de CLI.

CA-2.2 cumple el presupuesto de capacidad estructural: 14,248,912 bytes incluyendo
el objeto, excluidos embeddings, asignador y temporales. Los deltas RSS se
preservan con su variación, sin equipararlos a esa capacidad.
CA-2.3 no se midió: necesita Linux perf/PMU. No se declara aprobado todo el Paso 2.
La revisión del equipo y aprobación de PR son independientes.

Los arrays grandes y ejecutables no se duplican en el paquete; sus hashes,
tamaños y rutas locales constan en el manifiesto. Los datos DBpedia se verifican
contra su manifiesto original; tampoco se vuelven a copiar aquí.

Para auditar los artefactos y las fuentes del checkout:

```sh
python3 scripts/analyze_optimization_evidence.py --verify-current-source .
```
