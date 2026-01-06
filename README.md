PPF Manager
============

ES

Herramienta para crear y aplicar parches PPF en Windows.
Basado en las fuentes PPF3 de Icarus/Paradox, para que sea compatible con parches antiguos

Cambios relevantes:
- Interfaz gráfica para crear y aplicar los parches fácilmente, también se puede usar en modo consola.
- Soporte completo para UTF-8/ANSI según convenga, textos y file_id.diz salen legibles.
- Validación de bloque PPF3 intacta con aviso si no coincide.
- Creación de parches: escribe en archivo temporal y sólo renombra si termina bien (evita parches corruptos).
- file_id.diz se limita a 3 KB para no romper el parche.
- Gestión y limpieza de errores mejorada y más robusta.
- Corregidos errores menores que podían causar conflictos.

————————————————————————————————————————————

EN

Tool for creating and applying PPF patches on Windows.
Based on the Icarus/Paradox PPF3 source code, to make it compatible with older patches

Key changes:
- Graphical interface for easy patch creation and application; console mode is also available.
- Full support for UTF-8/ANSI as needed; text and file_id.diz are displayed legibly.
- Validation of intact PPF3 blocks with warnings if they don't match.
- Patch creation: writes to a temporary file and only renames if successful (prevents corrupted patches).
- file_id.diz is limited to 3 KB to avoid breaking the patch.
- Improved and more robust error handling and cleanup.
- Minor errors that could cause conflicts have been corrected.