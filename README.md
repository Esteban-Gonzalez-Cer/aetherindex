# AetherIndex

AetherIndex es un motor de búsqueda híbrida desarrollado en C++20 que combina
técnicas de búsqueda vectorial y recuperación de información.

El proyecto contempla los siguientes algoritmos:

- HNSW (Hierarchical Navigable Small World): búsqueda aproximada de vecinos
  más cercanos mediante índices vectoriales.
- BM25 (Best Matching 25): recuperación de documentos mediante índices
  invertidos y relevancia léxica.
- RRF (Reciprocal Rank Fusion): combinación de resultados provenientes
  de diferentes métodos de búsqueda.

El desarrollo se realiza de manera incremental mediante sprints.

La estructura de directorios y las responsabilidades de cada módulo están
documentadas en [Arquitectura del proyecto](docs/architecture/README.md).
Las carpetas reservadas para funcionalidades futuras contienen `.gitkeep`
para conservarse en Git; todavía no implementan algoritmos ni servicios.

`VectorStore` almacena embeddings contiguos con IDs `uint32_t`, dimensión fija
por instancia y acceso sin copia. `distance` implementa coseno y euclidiana al
cuadrado con kernels FP32/SIMD. Cosine normaliza al insertar; las filas están
alineadas a 64 bytes. BruteForce usa un heap Top-K y HNSW usa FlatGraph con
selección diversificada (Algoritmo 4). Fuerza bruta utiliza estos módulos a través de `QueryEngine`. La CLI
carga archivos CSV y coordina fuerza bruta o HNSW mediante `SearchService`, sin
incorporar lógica de almacenamiento ni algoritmos en la presentación.

Sprint 2 incorpora la biblioteca independiente `aether_lexical`: tokenizer,
índice invertido, estadísticas y BM25 con rankings de `DocumentId` y score double.
Su uso, contratos y pruebas están en [BM25 — CP-02](docs/algorithms/bm25.md).
CP-03 incorpora `aether_hybrid` para [fusión RRF](docs/algorithms/rrf.md) mediante
rankings de DocumentId, con pruebas matemáticas y de contrato. La composición
híbrida y la extensión de la CLI corresponden a CP-04. Las pruebas léxicas
conservan su fixture de regresión identificado por separado de CP-01.

La [validación de optimizaciones](docs/benchmarks/OPTIMIZATION_VALIDATION.md)
documenta los cambios de contrato, el presupuesto de memoria de 14.50 MB y
benchmarks optativos de Top-K, Recall y p99. Las evidencias anteriores del
Sprint 1 se conservan como baseline histórico.

## CLI mínima: inicio rápido

Requisitos: CMake 3.20 o superior y un compilador con C++20. Desde la raíz del
repositorio, en macOS/Linux con un generador de configuración única:

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cli --parallel 4
./build-cli/aetherindex --help
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --k 2
ctest --test-dir build-cli --output-on-failure
```

En Windows, después de configurar y compilar con Visual Studio como se explica
más adelante:

```powershell
.\build\Debug\aetherindex.exe --help
.\build\Debug\aetherindex.exe search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm brute-force --k 2
ctest --test-dir build -C Debug --output-on-failure
```

El ejemplo devuelve **ID 10 a distancia 0**, seguido de **ID 40 a distancia
aproximada 0.2**. La salida incluye algoritmo, métrica, tamaño, dimensión, K y
tiempo de consulta en milisegundos, seguido de una tabla con bordes y separadores
por fila. Los IDs son enteros y las distancias muestran nueve decimales, incluyendo
`0.000000000`; las distancias menores van primero. El tiempo mide la búsqueda y
excluye carga de CSV y presentación.

| Argumento | Contrato |
|---|---|
| `search` | Carga el dataset, prepara el almacenamiento y consulta en una ejecución |
| `--dataset FILE` | Obligatorio; CSV sin encabezado: `VectorId,x1,...,xD` |
| `--query FILE` | Obligatorio; una fila `x1,...,xD`, sin ID |
| `--algorithm NAME` | Obligatorio; `brute-force` exacto o `hnsw` aproximado |
| `--k INTEGER` | Entero positivo; por defecto 10; como máximo `min(K,N)` vecinos |
| `--metric NAME` | `cosine` por defecto, o `squared-euclidean` |
| `--m`, `--m0` | HNSW: límites de vecinos superiores/base; por defecto 16/32 |
| `--ef-construction`, `--ef-search` | HNSW: amplitud de construcción/consulta; por defecto 100/50 |
| `--seed INTEGER` | HNSW: semilla uint64 reproducible; por defecto 42, cero permitido |
| `--neighbor-selection NAME` | HNSW: `heuristic` (predeterminado, Algoritmo 4) o `simple` (Algoritmo 3) |
| `--help` | Ayuda; también `search --help` o ejecución sin argumentos |

Los IDs son enteros sin signo de 32 bits únicos, incluyendo 0. Todos los
embeddings deben tener la misma dimensión y componentes finitos. Se admiten
decimales, notación científica, espacios/tabulaciones alrededor de los campos,
líneas vacías y finales de línea LF/CRLF. No se admiten encabezados, comillas ni
campos vacíos. Coseno rechaza embeddings con norma L2 menor que 1e-7 y almacena su dirección
normalizada; la consulta debe ser no nula. Euclidiana al cuadrado conserva
magnitudes y permite ceros. Por ejemplo:

```csv
10,1.0,0.0
20,0.0,1.0
30,-1.0,0.0
40,0.8,0.6
```

Consulta correspondiente:

```csv
1.0,0.0
```

**HNSW está integrado** mediante la capa de aplicación. El siguiente comando
construye el grafo en memoria y ejecuta la consulta aproximada:

```sh
./build-cli/aetherindex search --dataset datasets/examples/vectors.csv --query datasets/examples/query.csv --algorithm hnsw --k 10
```

Para usar los datos reales ya preparados:

```sh
./build-cli-release/aetherindex search --dataset datasets/real/dbpedia/vectors.csv --query datasets/real/dbpedia/queries/query_00.csv --algorithm hnsw --k 10 --ef-search 100 --seed 42
```

La construcción se mide por separado y queda fuera de `Query time (ms)`.
Cada ejecución vuelve a cargar los datos y construir el grafo. HNSW conserva
IDs y topología; `VectorStore` conserva los embeddings. No se sustituye la
búsqueda aproximada por fuerza bruta. El resultado depende de la conectividad
y de los parámetros: configuraciones dispersas pueden recuperar menos de K
vecinos. Ver [el contrato de HNSW](docs/algorithms/hnsw.md).

Los resultados y la ayuda se escriben en `stdout`; los errores, en `stderr`.
Códigos: **0** éxito/ayuda, **2** argumentos o configuración inválidos,
**3** lectura/validación de datos, **4** error de búsqueda/interno, incluyendo
desbordamiento de una distancia. Una entrada inválida cancela toda la operación.
Los archivos deben ser archivos regulares legibles; no se admite entrada por
stdin. No hay persistencia: los datos se vuelven a cargar en cada ejecución.

Consultar [Uso y pruebas de la CLI](docs/cli.md) y
[Validación de la tarjeta](docs/architecture/cli_validation.md) para el dataset
reproducible de 1,000 × 128 y las consultas CLI. La
[auditoría y validación de HNSW](docs/architecture/hnsw_validation.md) registra
los incumplimientos de la versión recibida, las correcciones y la comparación
contra fuerza bruta con 100 consultas independientes por métrica.

El [informe de validación del Sprint 1 / OKR 1](docs/architecture/SPRINT_1_OKR1_VALIDATION.md)
reúne los indicadores KR1.1, KR1.2 y KR1.6, los tres reportes CSV y los pasos para
repetir la aceptación con 1,000 × 128 embeddings y 100 consultas independientes.

Para presentar esta versión, consultar el
[guion del showcase en español](docs/showcase/SPRINT_1_SHOWCASE.md) y la
[hoja de comandos para macOS](docs/showcase/COMANDOS_MACOS.md). Incluyen una
introducción a embeddings/LLMs, artículos reales, comparación exacta/HNSW y
salidas del ensayo para respaldo.

El [informe técnico del Sprint 1 para evaluación externa](docs/reports/INFORME_TECNICO_AETHERINDEX_SPRINT_1.md)
describe algoritmos, variantes de HNSW, complejidad, resultados comprobados,
limitaciones y mejoras prioritarias, con las evidencias reproducibles enlazadas.

El [caso real de recomendación de artículos](docs/real_world_validation.md) amplía
la validación a **10,000 artículos DBpedia × 1,024 dimensiones**, con 20 consultas
separadas y comparación exhaustiva contra NumPy. Usa embeddings públicos ya
calculados y el mismo formato CSV; no requiere generar embeddings ni usar una API
de pago. La descarga y el test de datos reales se activan explícitamente.

Consultar [VectorStore y distancias](docs/algorithms/vector_store_and_distances.md)
para el contrato y los comandos de pruebas, [Fuerza bruta](docs/algorithms/brute_force.md)
para el flujo de búsqueda y [Evidencia de validación](docs/architecture/vector_store_validation.md)
para los resultados registrados.

## 1. Requisitos del sistema

Para compilar y ejecutar AetherIndex en Windows se necesita:

### Sistema operativo

- Windows 10 o Windows 11 de 64 bits.
- Arquitectura x64.

### Herramientas

| Herramienta | Requisito | Propósito |
|---|---|---|
| Git | Versión reciente | Clonar el repositorio |
| CMake | 3.20 o superior | Configuración del proyecto |
| Visual Studio | 2022 | Entorno de compilación |
| MSVC | Compatible con C++20 | Compilador de C++ |
| Windows SDK | Incluido con Visual Studio | Bibliotecas y herramientas de Windows |

**Importante:** Visual Studio Code no sustituye al compilador MSVC.
Es necesario instalar las herramientas de compilación de C++.

## 2. Clonar el repositorio

Abrir PowerShell o Windows Terminal.

Ubicarse en el directorio donde se desea descargar el proyecto.

Ejemplo:

```powershell
cd "$HOME\Documents"
```

Clonar el repositorio:

```powershell
git clone "https://github.com/ORGANIZACION/AetherIndex.git"
```

Sustituir la URL por la dirección real del repositorio.

Una vez terminada la descarga, acceder al directorio:

```powershell
cd AetherIndex
```

Verificar que la descarga se realizó correctamente:

```powershell
git status
```

El comando debería indicar que nos encontramos dentro de un repositorio
Git y mostrar el estado actual de los archivos.

También se puede verificar la estructura:

```powershell
Get-ChildItem
```

Es necesario comprobar que existe el archivo:

```text
CMakeLists.txt
```

Este archivo contiene la configuración utilizada por CMake para
generar el sistema de compilación.

## 3. Instalación de herramientas

### 3.1. Verificar Git

Ejecutar:

```powershell
git --version
```

Ejemplo de salida:

```text
git version 2.x.x.windows.x
```

Si el comando funciona, Git está instalado correctamente.

Si no está disponible, descargarlo desde:

https://git-scm.com/downloads/win

### 3.2. Verificar CMake

Ejecutar:

```powershell
cmake --version
```

Ejemplo de salida:

```text
cmake version 4.x.x
```

Si CMake no está instalado, ejecutar:

```powershell
winget install --id Kitware.CMake -e
```

Al finalizar, cerrar y volver a abrir PowerShell.

Verificar nuevamente:

```powershell
cmake --version
```

### 3.3. Instalar Visual Studio 2022

Descargar Visual Studio 2022 Community desde:

https://visualstudio.microsoft.com/vs/older-downloads/

Durante la instalación, seleccionar la carga de trabajo:

**Desktop development with C++**

Verificar que estén seleccionados los siguientes componentes:

- MSVC v143 - VS 2022 C++ x64/x86 build tools.
- Windows 10 SDK o Windows 11 SDK.
- C++ CMake tools for Windows.

Los nombres y versiones exactos pueden variar según la instalación.

Una vez instalados los componentes, cerrar y volver a abrir
PowerShell.

## 4. Configurar el proyecto con CMake

Asegurarse de estar ubicado en el directorio raíz de AetherIndex.

```powershell
cd "$HOME\Documents\AetherIndex"
```

La ruta anterior es un ejemplo. Utilizar la ubicación donde realmente
se clonó el proyecto.

Verificar que existe el archivo de configuración:

```powershell
Test-Path .\CMakeLists.txt
```

Resultado esperado:

```text
True
```

### 4.1. Configuración inicial

Ejecutar:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

Descripción de los parámetros:

- `-S .`: utiliza el directorio actual como origen del código.
- `-B build`: genera los archivos de compilación en la carpeta build.
- `-G "Visual Studio 17 2022"`: selecciona el generador de Visual Studio 2022.
- `-A x64`: configura la compilación para arquitectura de 64 bits.

CMake comenzará a detectar las herramientas disponibles.

Ejemplo de salida:

```text
-- The CXX compiler identification is MSVC ...
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: ...
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Configuring done
-- Generating done
-- Build files have been written to: .../AetherIndex/build
```

La configuración se considera exitosa cuando aparecen:

```text
-- Configuring done
-- Generating done
```

Y el comando termina con código de salida 0.

### 4.2. Verificar archivos generados

Ejecutar:

```powershell
Get-ChildItem .\build
```

Deberían aparecer archivos similares a:

```text
build/
├── CMakeCache.txt
├── CMakeFiles/
├── AetherIndex.sln
└── ...
```

El nombre del archivo de solución dependerá de la configuración del
proyecto.

## 5. Compilación del proyecto

Una vez configurado CMake, proceder con la compilación.

### 5.1. Compilar en modo Debug

Ejecutar desde la raíz del proyecto:

```powershell
cmake --build build --config Debug
```

Este comando utiliza MSVC para compilar los archivos fuente del proyecto.

Durante la compilación aparecerán mensajes indicando los archivos
procesados y los targets generados.

La compilación se considera exitosa si:

1. No existen errores de compilación.
2. El proceso termina correctamente.
3. Se genera el ejecutable correspondiente.

### 5.2. Verificar el resultado

Inmediatamente después de compilar, ejecutar:

```powershell
$LASTEXITCODE
```

Resultado esperado:

```text
0
```

Un código de salida 0 indica que el proceso de compilación finalizó
satisfactoriamente.

### 5.3. Compilar en modo Release

Opcionalmente, generar una versión optimizada:

```powershell
cmake --build build --config Release
```

Diferencias principales:

- Debug: orientado a depuración y diagnóstico.
- Release: orientado a ejecución optimizada.

En el generador de Visual Studio, ambas configuraciones pueden coexistir
dentro del mismo directorio build.

## 6. Ejecución del programa

Una vez compilado el proyecto, localizar el ejecutable generado.

### 6.1. Buscar el ejecutable

Ejecutar:

```powershell
Get-ChildItem -Path .\build -Recurse -Filter *.exe
```

Este comando mostrará los archivos ejecutables disponibles dentro
del directorio de compilación.

Identificar el ejecutable correspondiente a AetherIndex.

Si el target se llama `aetherindex` y utiliza las rutas predeterminadas
del generador de Visual Studio, la ubicación probablemente será:

```text
build/Debug/aetherindex.exe
```

La ubicación exacta dependerá de la configuración de CMake.

### 6.2. Ejecutar AetherIndex

Si el ejecutable se encuentra en la ruta anterior:

```powershell
.\build\Debug\aetherindex.exe
```

El programa deberá iniciar y ejecutar las funcionalidades
implementadas en la versión actual.

### 6.3. Verificar la finalización

Después de que el programa termine, ejecutar:

```powershell
$LASTEXITCODE
```

Resultado esperado:

```text
0
```

Este resultado confirma que el proceso terminó correctamente.

Si el programa requiere argumentos o interacción del usuario,
seguir las instrucciones correspondientes a su versión.

## 7. Pruebas de reproducibilidad

Esta sección permite verificar que AetherIndex puede configurarse,
compilarse y ejecutarse desde una instalación limpia.

El procedimiento debe realizarlo un integrante del equipo que no
haya preparado previamente el entorno del proyecto.

### 7.1. Verificar el repositorio

Desde la raíz del proyecto:

```powershell
git status
```

Registrar la versión del código utilizada:

```powershell
git rev-parse HEAD
```

### 7.2. Eliminar compilaciones anteriores

Si existe un directorio build generado anteriormente, eliminarlo:

```powershell
Remove-Item -Recurse -Force .\build
```

**Advertencia:** este comando elimina completamente el directorio
build. Ejecutarlo únicamente desde la raíz correcta del proyecto
y después de verificar que no contiene archivos que deban conservarse.

Si se acaba de clonar el repositorio y no existe build, este paso
puede omitirse.

### 7.3. Configurar nuevamente

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

Comprobar que la configuración finalice sin errores.

### 7.4. Compilar nuevamente

```powershell
cmake --build build --config Debug
```

Comprobar que se genere el ejecutable.

### 7.5. Ejecutar nuevamente

Ejecutar el binario correspondiente:

```powershell
.\build\Debug\aetherindex.exe
```

Verificar que el programa inicie y termine correctamente.

### 7.6. Resultados esperados

La prueba será satisfactoria cuando:

- El repositorio pueda clonarse correctamente.
- Las dependencias estén instaladas y documentadas.
- CMake detecte un compilador compatible.
- Se configure el proyecto sin errores.
- El código compile correctamente.
- Se genere el ejecutable.
- El programa pueda ejecutarse.
- El programa termine sin fallos inesperados.

Si alguno de los pasos falla, registrar el error y comunicarlo
al equipo de desarrollo.

## 8. Problemas frecuentes

### Error: 'cmake' no se reconoce como comando

Causa probable:

CMake no está instalado o su directorio no está incluido en PATH.

Solución:

Instalar CMake y reiniciar la terminal.

```powershell
winget install --id Kitware.CMake -e
```

### Error: Could not find any instance of Visual Studio

Causa probable:

Visual Studio 2022 no está instalado correctamente o faltan
componentes necesarios.

Solución:

Abrir Visual Studio Installer y verificar que esté instalada la carga
de trabajo:

Desktop development with C++.

Comprobar también la disponibilidad de MSVC y Windows SDK.

### Error: No CMAKE_CXX_COMPILER could be found

Causa probable:

No existe un compilador de C++ disponible para la configuración
seleccionada.

Solución:

Instalar las herramientas de compilación de C++ desde
Visual Studio Installer.

Después, eliminar el directorio build e intentar nuevamente.

### Error: existen referencias a bibliotecas de macOS o Linux

Causa probable:

Alguna parte del código utiliza APIs específicas de sistemas POSIX
que no están disponibles de forma nativa en Windows.

Solución:

Revisar el archivo y la funcionalidad que producen el error.

Es posible que se requiera una implementación específica para Windows
o una abstracción multiplataforma.

No sustituir directamente bibliotecas sin verificar su
compatibilidad con el diseño del proyecto.

### Error: no se encuentra el ejecutable

Causa probable:

El archivo fue generado en un directorio diferente.

Solución:

```powershell
Get-ChildItem -Path .\build -Recurse -Filter *.exe
```

Verificar la ruta real y ejecutar el binario correspondiente.

## 9. Registro de evidencias

Para documentar la prueba de reproducibilidad se recomienda capturar
los siguientes resultados:

1. Versión de Git, CMake y herramientas de compilación.
2. Repositorio clonado correctamente.
3. Configuración exitosa mediante CMake.
4. Compilación exitosa mediante MSVC.
5. Ejecutable generado.
6. Ejecución del programa.
7. Finalización sin errores.

Cada evidencia debe registrar:

- Fecha de ejecución.
- Integrante responsable.
- Sistema operativo utilizado.
- Comando ejecutado.
- Resultado obtenido.
- Observaciones, si existen.

## 10. Criterio de aceptación

La prueba de reproducibilidad se considera aprobada cuando otro
integrante puede seguir las instrucciones de este README para
configurar, compilar y ejecutar AetherIndex sin depender de
configuraciones locales no documentadas.

La compatibilidad con Windows deberá confirmarse mediante una
ejecución real de estas pruebas.
