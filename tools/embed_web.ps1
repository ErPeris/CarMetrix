# ============================================================
#  CarMetrix — embed_web.ps1  (equivalente PowerShell di embed_web.py,
#  per macchine senza Python)
#  Comprime carmetrix/data/index.html (gzip) in un header C PROGMEM:
#    carmetrix/src/web_index.h
#
#  Eseguire DOPO ogni modifica all'HTML e PRIMA di compilare,
#  così la web UI viene inclusa nel firmware e si aggiorna via OTA.
#
#  Uso:  powershell -File tools/embed_web.ps1
# ============================================================

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'web\index.html'
$out  = Join-Path $root 'carmetrix\src\web_index.h'

$raw = [System.IO.File]::ReadAllBytes($src)

$ms = New-Object System.IO.MemoryStream
$gzs = New-Object System.IO.Compression.GzipStream($ms, [System.IO.Compression.CompressionLevel]::Optimal)
$gzs.Write($raw, 0, $raw.Length)
$gzs.Close()
$gz = $ms.ToArray()
$ms.Close()

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('// ============================================================')
[void]$sb.AppendLine('//  AUTO-GENERATO da tools/embed_web.ps1 (o embed_web.py) — NON modificare a mano.')
[void]$sb.AppendLine('//  Rigenerare con:  powershell -File tools/embed_web.ps1')
[void]$sb.AppendLine('//  (dopo aver modificato carmetrix/data/index.html)')
[void]$sb.AppendLine('// ============================================================')
[void]$sb.AppendLine("// HTML: $($raw.Length) byte  ->  gzip: $($gz.Length) byte")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const unsigned char INDEX_HTML_GZ[] PROGMEM = {')
for ($i = 0; $i -lt $gz.Length; $i += 16) {
  $end = [Math]::Min($i + 15, $gz.Length - 1)
  $hex = ($gz[$i..$end] | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
  [void]$sb.AppendLine("  $hex,")
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine("const unsigned int INDEX_HTML_GZ_LEN = $($gz.Length);")

# LF, niente BOM (come l'originale Python)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($out, $sb.ToString().Replace("`r`n", "`n"), $utf8)

Write-Host "OK: index.html $($raw.Length) B  ->  web_index.h (gzip $($gz.Length) B)"
