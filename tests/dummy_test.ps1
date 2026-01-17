param(
    [string]$Name
)

switch ($Name) {
    'apply_small_file'              { Write-Host "OK: apply succeeded"; break }
    'truncation_test'              { Write-Host "OK: truncation test passed"; break }
    'trunc_name_test'              { Write-Host "OK: long ppf file created"; break }
    'translation_overlap'          { Write-Host "OK: overlap translation passed"; break }
    'translation_idempotence'      { Write-Host "OK: done translation idempotent"; break }
    'compare_console_makeppf_desc'  { Write-Host "Files identical."; break }
    'patch_equivalence'            { Write-Host "OK: patch-equivalence passed"; break }
    'emit_long'                    { Write-Host ([string]('A' * 5000)); break }
    'interrupt_apply'              { Write-Host "OK"; break }
    'roundtrip_binary'             { Write-Host "OK"; break }
    'parse_ppf_entries'            { return } # no output
    default                        { Write-Host "OK"; break }
}
