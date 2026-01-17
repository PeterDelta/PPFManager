# Comprueba que los recursos necesarios estén presentes
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $repoRoot "..")
$resources = Join-Path $repoRoot "resources"
$ok = $true

Write-Host "Comprobando recursos en: $resources"

$icon = Join-Path $resources "PPFManager.ico"
$manifest = Join-Path $resources "PPFManager.manifest"
$rc = Join-Path $resources "PPFManager1.rc"
$resObj = Join-Path $resources "PPFManager_res.o"

if (Test-Path $icon) { Write-Host "[OK] Icono encontrado: $icon" } else { Write-Host "[FAIL] Icono NO encontrado: $icon"; $ok = $false }
if (Test-Path $manifest) { Write-Host "[OK] Manifest encontrado: $manifest" } else { Write-Host "[FAIL] Manifest NO encontrado: $manifest"; $ok = $false }
if (Test-Path $rc) { Write-Host "[OK] RC encontrado: $rc" } else { Write-Host "[FAIL] RC NO encontrado: $rc"; $ok = $false }
if (Test-Path $resObj) { Write-Host "[OK] Recurso compilado: $resObj" } else { Write-Host "[WARN] Recurso compilado no encontrado (es posible que no se haya compilado todavía): $resObj" }

# Check rc contains correct icon and manifest lines
$rcContent = Get-Content $rc -ErrorAction SilentlyContinue
if ($rcContent -match '101 ICON "PPFManager.ico"') { Write-Host "[OK] RC referencia icono" } else { Write-Host "[FAIL] RC no referencia icono correctamente"; $ok = $false }
if ($rcContent -match '1 24 "PPFManager.manifest"') { Write-Host "[OK] RC referencia manifest" } else { Write-Host "[FAIL] RC no referencia manifest correctamente"; $ok = $false }

if ($ok) { Write-Host "Comprobacion de recursos: OK"; exit 0 } else { Write-Host "Comprobacion de recursos: FALLO"; exit 2 }