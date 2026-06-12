# Mini server statico per la preview locale della web UI (data/index.html).
# Solo per sviluppo: le /api/* rispondono 404 e la UI degrada con grazia.
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot "..\web"
$listener = New-Object Net.HttpListener
$listener.Prefixes.Add("http://localhost:8765/")
$listener.Start()
Write-Host "Serving $root on http://localhost:8765/"
while ($true) {
  $ctx = $listener.GetContext()
  try {
    $path = $ctx.Request.Url.AbsolutePath
    if ($path -eq "/") { $path = "/index.html" }
    $file = Join-Path $root $path.TrimStart('/')
    if ((Test-Path $file) -and -not (Get-Item $file).PSIsContainer) {
      $bytes = [IO.File]::ReadAllBytes($file)
      if ($file -like "*.html") { $ctx.Response.ContentType = "text/html; charset=utf-8" }
      elseif ($file -like "*.json") { $ctx.Response.ContentType = "application/json" }
      $ctx.Response.ContentLength64 = $bytes.Length
      if ($ctx.Request.HttpMethod -ne 'HEAD') {
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      }
    } else {
      $ctx.Response.StatusCode = 404
    }
  } catch {
    try { $ctx.Response.StatusCode = 500 } catch {}
  }
  try { $ctx.Response.Close() } catch {}
}
