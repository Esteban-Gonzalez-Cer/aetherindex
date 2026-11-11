# Evidencias del checklist híbrido — 2026-10-30

Cada archivo .log contiene el comando exacto, código esperado/real, stdout y stderr.
commands.json indexa las ejecuciones; build-commands.json conserva la compilación inicial.
verification-summary.json resume los once controles, 24 consultas, 720 filas fusionadas y 240 filas Top-10.

El informe principal está en docs/reports/HYBRID_CLI_TESTING_CHECKLIST_2026-10-30.md.
Los comandos se ejecutaron desde la raíz de AetherIndex; no se incluyen los binarios ni todo el repositorio en este paquete.
Los inputs/ son copias exactas para inspección; fixtures/ contiene entradas negativas que deben fallar.
El wrapper record_command.py conserva las salidas de cada comando sin usar un shell.
La implementación y los tests existentes no se modificaron.
