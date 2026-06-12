# ============================================================
#  CarMetrix — obdb_to_profile.ps1  (v3)
#  Converte i signalset OBDb (github.com/OBDb, un repo per modello)
#  in profili CarMetrix schema 2, organizzati in cartelle per marca:
#      data/profiles/<marca>/<modello>.json   (minificati)
#  e genera data/profiles/index.json per la selezione guidata della web app.
#
#  Uso:
#    Tutto l'org:  powershell -File tools/obdb_to_profile.ps1 -All
#    Singolo:      powershell -File tools/obdb_to_profile.ps1 -Repo Honda-Civic [-Out honda/civic.json]
#    Per marca:    powershell -File tools/obdb_to_profile.ps1 -Brands BMW,Ford
#    Solo indice:  powershell -File tools/obdb_to_profile.ps1 -IndexOnly
#
#  Whitelist gauge CarMetrix:
#    oil_temp / trans_temp / boost (kPa->bar) / hv_soc / hv_temp (ibride)
#  Scarta: manutenzioni, celle, contatori, segnali oltre 64 byte payload,
#  modelli scheletro (la maggioranza dei 740 repo OBDb).
#  Budget LittleFS: 80KB. Le marche prioritarie entrano per prime, le altre
#  finché c'è spazio; gli esclusi vengono riportati a video.
#
#  I profili scritti A MANO (es. bmw/1_series.json, toyota/yaris_hybrid.json)
#  NON vengono toccati: -All salta i file esistenti non generati da OBDb
#  (campo "source" che non inizia per "OBDb/").
#
#  Mappatura manuale iternio/ev-obd-pids e WICAN (fase 2, formule Torque):
#    lettere = indice byte payload: A=0,B=1..Z=25,AA=26..AZ=51,BA=52...
#    "(A*256+B)/k" -> bix=A*8, len=16, div=k ; "Signed(X)" -> sign=true
#    WICAN "[B4:B5]/16" -> byte 4..5 big-endian, div=16 (convenzione B0 da
#    verificare!); i pid_init WICAN contengono anche ATFCSH (flow control:
#    non ancora emesso dal nostro firmware — aggiungere se servono Stellantis EV).
# ============================================================
param(
  [switch]   $All,
  [string]   $Repo,
  [string[]] $Brands,
  [string]   $Out,
  [switch]   $IndexOnly,
  [int]      $MaxTotalKB = 60
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repoRoot    = Split-Path -Parent $PSScriptRoot
# SORGENTI (git, leggibili): profiles/<marca>/<modello>.json
$profilesDir = Join-Path $repoRoot 'profiles'
# OUTPUT per il device (LittleFS): UN SOLO bundle + indice.
# NB: LittleFS alloca minimo 1 blocco da 4KB per file → centinaia di
# profili separati NON entrano nei 128KB. Per questo si bundla tutto.
$bundlePath  = Join-Path $repoRoot 'carmetrix\data\profiles.json'
$indexPath   = Join-Path $repoRoot 'carmetrix\data\profiles_index.json'
$MAX_BITS = 512   # 64 byte payload (ELM_RESP_MAX nel firmware)

# Marche con nome a due token nei nomi repo OBDb
$TWO_TOKEN = @('Mercedes-Benz','Land-Rover','Alfa-Romeo','Aston-Martin','Rolls-Royce','Great-Wall')
# Marche prioritarie (auto testabili dall'utente) — entrano sempre nel budget
$PRIORITY = @('honda','bmw','ford','toyota','fiat','mercedes-benz','vauxhallopel','citroen','opel')

function Split-RepoName($repoName) {
  foreach ($t in $TWO_TOKEN) {
    # repo brand-wide senza modello (es. "Mercedes-Benz", "Land-Rover")
    if ($repoName -eq $t) { return @{ brand = $t; model = 'All' } }
    if ($repoName -like "$t-*") {
      return @{ brand = $t; model = $repoName.Substring($t.Length + 1) }
    }
  }
  $i = $repoName.IndexOf('-')
  if ($i -lt 1) { return @{ brand = $repoName; model = 'All' } }
  return @{ brand = $repoName.Substring(0, $i); model = $repoName.Substring($i + 1) }
}

# ── Classificazione segnale -> target CarMetrix ──────────────
function Classify($sig) {
  $n = "$($sig.id) $($sig.name) $($sig.path)".ToLower()
  $u = "$($sig.fmt.unit)".ToLower()
  # non-gauge: contatori, manutenzioni, celle, limiti, 12V
  if ($n -match 'maintenance|maint_|service|cell|limit|target|count|12v') { return $null }
  if ($n -match 'transmission fluid|atf|gearbox.*temp|transmission.*temp|cvt.*temp') {
    return @{ target='trans_temp'; name='TRANS'; unit=([char]0xB0 + 'C') }
  }
  if ($n -match '(engine )?oil temp|oil temperature') {
    return @{ target='oil_temp'; name='OIL'; unit=([char]0xB0 + 'C') }
  }
  if (($n -match 'boost|charge air|turbo.*pressure') -and ($u -match 'kilopascal|bars|psi|pascal')) {
    return @{ target='boost'; name='BOOST'; unit='bar' }
  }
  # Ibride/EV: stato di carica HV (esclude min/max ridondanti)
  if (($n -match 'state of charge|hv battery charge|\bsoc\b') -and ($u -eq 'percent') -and
      ($n -notmatch 'min|max|display')) {
    return @{ target='hv_soc'; name='SOC'; unit='%' }
  }
  # Ibride/EV: temperatura batteria HV / inverter
  if (($n -match 'hv batt.*temp|battery pack.*temp|inverter.*temp') -and ($u -eq 'celsius')) {
    return @{ target='hv_temp'; name='HV TEMP'; unit=([char]0xB0 + 'C') }
  }
  return $null
}

# ── Converte un signalset OBDb in profilo schema 2 ───────────
function Convert-Signalset($json, $brandLabel, $repoName) {
  $pids = @()
  $seenTargets = @{}
  foreach ($c in $json.commands) {
    $cmdProp = $c.cmd.PSObject.Properties | Select-Object -First 1
    if (-not $cmdProp) { continue }
    $mode   = $cmdProp.Name        # "22" o "21"
    $pidHex = $cmdProp.Value
    if ($mode -ne '22' -and $mode -ne '21') { continue }
    foreach ($s in $c.signals) {
      $cls = Classify $s
      if ($null -eq $cls) { continue }
      if ($seenTargets.ContainsKey($cls.target)) { continue }  # 1 segnale per target
      $f = $s.fmt
      $bix = 0; if ($f.bix) { $bix = [int]$f.bix }
      $len = 8; if ($f.len) { $len = [int]$f.len }
      if (($bix + $len) -gt $MAX_BITS) { continue }
      if ($len -gt 32) { continue }

      $mul = 1.0; if ($f.mul) { $mul = [double]$f.mul }
      $div = 1.0; if ($f.div) { $div = [double]$f.div }
      $add = 0.0; if ($f.add) { $add = [double]$f.add }
      $scale = $mul / $div
      $sign  = [bool]$f.sign

      if ($cls.target -eq 'boost') {
        switch -Regex ("$($f.unit)".ToLower()) {
          'kilopascal' { $scale = $scale / 100.0; $add = $add / 100.0 - 1.013 }
          'pascal'     { $scale = $scale / 100000.0; $add = $add / 100000.0 - 1.013 }
          'psi'        { $scale = $scale / 14.5038; $add = $add / 14.5038 }
        }
      }

      $defMin = -40; $defMax = 215
      if ($cls.target -eq 'boost')  { $defMin = -1; $defMax = 3 }
      if ($cls.target -eq 'hv_soc') { $defMin = 0;  $defMax = 100 }
      $min = $defMin; if ($null -ne $f.min) { $min = [double]$f.min }
      $max = $defMax; if ($null -ne $f.max) { $max = [double]$f.max }

      $every = 4
      if ($c.freq -and [double]$c.freq -ge 5) { $every = 1 }
      elseif ($c.freq -and [double]$c.freq -lt 1) { $every = 8 }

      $entry = [ordered]@{
        cmd  = "$mode$pidHex"
        hdr  = "$($c.hdr)"
        name = $cls.name
        unit = $cls.unit
        target = $cls.target
        bix  = $bix
        len  = $len
        mul  = [Math]::Round($scale, 8)
        add  = [Math]::Round($add, 4)
        min  = $min
        max  = $max
        every = $every
        src  = "$($s.id)"
      }
      if ($c.rax)  { $entry.rax = "$($c.rax)" }
      if ($sign)   { $entry.sign = $true }
      $pids += $entry
      $seenTargets[$cls.target] = $true
    }
  }
  if ($pids.Count -eq 0) { return $null }

  $proto = '0'
  $h0 = $pids[0].hdr
  if ($h0 -match '^DA') { $proto = '7' } elseif ($h0 -match '^7') { $proto = '6' }

  return [ordered]@{
    schema   = 2
    brand    = $brandLabel
    source   = "OBDb/$repoName"
    protocol = $proto
    pids     = $pids
  }
}

function Get-Signalset($repoName) {
  $url = "https://raw.githubusercontent.com/OBDb/$repoName/main/signalsets/v3/default.json"
  try { return Invoke-RestMethod $url } catch { return $null }
}

function Save-Profile($obj, $relPath) {
  $path = Join-Path $profilesDir $relPath
  $dir = Split-Path -Parent $path
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  $jsonTxt = $obj | ConvertTo-Json -Depth 6 -Compress
  $utf8 = New-Object System.Text.UTF8Encoding($false)
  [IO.File]::WriteAllText($path, $jsonTxt, $utf8)
  return (Get-Item $path).Length
}

# ── Bundle device + indice: da TUTTI i sorgenti in profiles/ ─────
# Emette carmetrix/data/profiles.json (un oggetto: id → profilo snello,
# letto dal firmware con il filtro ArduinoJson) e profiles_index.json
# (lista {file,brand,model,extras,proto} per la selezione guidata web).
function Build-Bundle {
  $items = @()
  $files = Get-ChildItem $profilesDir -Filter *.json -Recurse | Sort-Object FullName
  foreach ($f in $files) {
    $rel = $f.FullName.Substring($profilesDir.Length + 1) -replace '\\', '/'
    $id  = $rel -replace '\.json$', ''
    try { $j = Get-Content $f.FullName -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }

    # Profilo snello per il device (src/source restano solo nei sorgenti git)
    $slim = [ordered]@{ schema = $j.schema; protocol = "$($j.protocol)" }
    if ($j.pids) {
      $slim.pids = @($j.pids | ForEach-Object {
        $o = [ordered]@{}
        foreach ($k in 'cmd','id','hdr','rax','name','unit','target','formula',
                       'bix','len','sign','mul','div','add','min','max','every') {
          if ($null -ne $_.$k -and "$($_.$k)" -ne '') { $o[$k] = $_.$k }
        }
        $o
      })
    }
    $bytes = [Text.Encoding]::UTF8.GetByteCount(("$id" + (($slim | ConvertTo-Json -Depth 5 -Compress))))

    # Marca/modello per l'indice
    $brand = "$($j.brand)"
    $bTok = $brand; $mTok = ''
    foreach ($t in ($TWO_TOKEN | ForEach-Object { $_ -replace '-', ' ' })) {
      if ($brand -like "$t *") { $bTok = $t; $mTok = $brand.Substring($t.Length + 1); break }
    }
    if ($mTok -eq '' -and $bTok -eq $brand) {
      $i = $brand.IndexOf(' ')
      if ($i -gt 0) { $bTok = $brand.Substring(0, $i); $mTok = $brand.Substring($i + 1) }
    }
    if ($mTok -eq 'All') { $mTok = 'Tutta la gamma' }
    $extras = @()
    if ($j.pids) { $extras = @($j.pids | ForEach-Object { "$($_.name)" } | Select-Object -Unique) }

    $items += @{
      id = $id; slim = $slim; bytes = $bytes
      prio  = [int]($PRIORITY -contains ($id -split '/')[0]) * 2 + [int]($id -eq 'generic')
      entry = [ordered]@{
        file   = $id
        brand  = if ($bTok) { $bTok } else { $id }
        model  = if ($mTok) { $mTok } else { 'Standard' }
        extras = $extras
        proto  = "$($j.protocol)"
      }
    }
  }

  # Budget device: bundle entro MaxTotalKB (prio: generic + marche utente)
  $budget = $MaxTotalKB * 1024
  $bundle = [ordered]@{}
  $entries = @(); $size = 2; $skipped = @()
  foreach ($it in ($items | Sort-Object { -$_.prio }, { $_.id })) {
    if (($size + $it.bytes + 8) -gt $budget) { $skipped += $it.id; continue }
    $bundle[$it.id] = $it.slim
    $entries += $it.entry
    $size += $it.bytes + 8
  }
  $entries = $entries | Sort-Object { $_.brand }, { $_.model }

  $utf8 = New-Object System.Text.UTF8Encoding($false)
  [IO.File]::WriteAllText($bundlePath, ($bundle | ConvertTo-Json -Depth 6 -Compress), $utf8)
  [IO.File]::WriteAllText($indexPath, ($entries | ConvertTo-Json -Depth 4 -Compress), $utf8)
  $bk = [Math]::Round((Get-Item $bundlePath).Length / 1024, 1)
  $ik = [Math]::Round((Get-Item $indexPath).Length / 1024, 1)
  Write-Host "profiles.json (bundle device): $($bundle.Count) profili, ${bk}KB" -ForegroundColor Green
  Write-Host "profiles_index.json: ${ik}KB" -ForegroundColor Green
  if ($skipped.Count) {
    Write-Host "Esclusi dal bundle per budget (${MaxTotalKB}KB): $($skipped -join ', ')" -ForegroundColor Red
  }
}

# ── Cataloga i profili manuali esistenti (da non sovrascrivere) ──
function Get-ManualProfiles {
  $manual = @{}
  Get-ChildItem $profilesDir -Filter *.json -Recurse |
    Where-Object { $_.Name -ne 'index.json' } | ForEach-Object {
      try {
        $j = Get-Content $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ("$($j.source)" -notlike 'OBDb/*') {
          $rel = $_.FullName.Substring($profilesDir.Length + 1) -replace '\\', '/'
          $manual[$rel] = $true
        }
      } catch {}
    }
  return $manual
}

# ============================================================
if ($IndexOnly) { Build-Bundle; exit 0 }

if ($Repo) {
  Write-Host "OBDb/$Repo..."
  $j = Get-Signalset $Repo
  if (-not $j) { Write-Error "signalset non trovato per $Repo"; exit 1 }
  $parts = Split-RepoName $Repo
  $brandLabel = "$($parts.brand -replace '-', ' ') $($parts.model -replace '-', ' ')".Trim()
  $prof = Convert-Signalset $j $brandLabel $Repo
  if (-not $prof) { Write-Host "  nessun segnale gauge utile" -ForegroundColor DarkYellow; exit 0 }
  if (-not $Out) {
    $Out = "$($parts.brand.ToLower())/$(($parts.model.ToLower() -replace '-','_')).json"
  }
  $sz = Save-Profile $prof $Out
  Write-Host "  -> $Out ($($prof.pids.Count) PID, $sz B)" -ForegroundColor Green
  Build-Bundle
  exit 0
}

if ($All -or $Brands) {
  if ($Brands) { $Brands = $Brands | ForEach-Object { $_ -split ',' } | Where-Object { $_ } }
  Write-Host "Elenco repo org OBDb..."
  $repos = @()
  for ($page = 1; $page -le 20; $page++) {
    $batch = Invoke-RestMethod "https://api.github.com/orgs/OBDb/repos?per_page=100&page=$page"
    if (-not $batch -or $batch.Count -eq 0) { break }
    $repos += $batch.name
  }
  Write-Host "$($repos.Count) repo totali"

  $manual = Get-ManualProfiles
  if ($manual.Count) { Write-Host "Profili manuali preservati: $($manual.Keys -join ', ')" }

  # Converte tutto in memoria, poi scrive secondo priorità e budget
  $candidates = @()
  foreach ($r in ($repos | Sort-Object)) {
    $parts = Split-RepoName $r
    if ($Brands -and -not ($Brands | Where-Object { $parts.brand -like $_ })) { continue }
    if ($parts.model -eq '') { continue }   # repo meta (.github ecc.)
    $j = Get-Signalset $r
    if (-not $j) { continue }
    $brandLabel = "$($parts.brand -replace '-', ' ') $($parts.model -replace '-', ' ')".Trim()
    $prof = Convert-Signalset $j $brandLabel $r
    if (-not $prof) { continue }
    $rel = "$($parts.brand.ToLower())/$(($parts.model.ToLower() -replace '-','_')).json"
    if ($manual.ContainsKey($rel)) { Write-Host "  $r -> manuale, salto"; continue }
    $bytes = [Text.Encoding]::UTF8.GetByteCount(($prof | ConvertTo-Json -Depth 6 -Compress))
    $candidates += @{ rel = $rel; prof = $prof; bytes = $bytes
                      prio = [int]($PRIORITY -contains $parts.brand.ToLower()) }
    Write-Host "  $r : $($prof.pids.Count) PID utili ($bytes B)"
  }

  # I sorgenti si scrivono TUTTI (sono solo nel git, non sul device);
  # il budget device lo applica Build-Bundle sul bundle unico.
  foreach ($c in $candidates) { Save-Profile $c.prof $c.rel | Out-Null }
  Write-Host "`nSorgenti scritti: $($candidates.Count) in profiles/" -ForegroundColor Cyan
  Build-Bundle
  exit 0
}

Write-Host "Specificare -All, -Repo Nome-Repo, -Brands Marca1,Marca2 o -IndexOnly"
