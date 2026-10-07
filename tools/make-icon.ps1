# Generates res/lightview.ico: a white page with a folded corner on a blue rounded square.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
param([string]$Out = (Join-Path $PSScriptRoot '..\res\lightview.ico'))

Add-Type -AssemblyName System.Drawing

function pt($x, $y) { New-Object System.Drawing.PointF ([float]$x), ([float]$y) }
function rgb($r, $g, $b) { [System.Drawing.Color]::FromArgb(255, $r, $g, $b) }

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = @()
foreach ($s in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)

  # Background: rounded square with a vertical gradient.
  $m = [float]($s * 0.03); $w = [float]($s - 2 * $m); $r = [float]($s * 0.2)
  $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
  $bg.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
  $bg.AddArc($m + $w - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
  $bg.AddArc($m + $w - 2 * $r, $m + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
  $bg.AddArc($m, $m + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
  $bg.CloseFigure()
  $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (pt 0 0), (pt 0 $s), (rgb 0x3B 0x82 0xF6), (rgb 0x1D 0x4E 0xD8)
  $g.FillPath($grad, $bg)

  # Page with a folded top-right corner.
  $px = $s * 0.27; $py = $s * 0.17; $pw = $s * 0.46; $ph = $s * 0.66; $f = $s * 0.17
  $page = New-Object System.Drawing.Drawing2D.GraphicsPath
  $page.AddLines([System.Drawing.PointF[]]@((pt $px $py), (pt ($px + $pw - $f) $py), (pt ($px + $pw) ($py + $f)),
                                             (pt ($px + $pw) ($py + $ph)), (pt $px ($py + $ph))))
  $page.CloseFigure()
  $g.FillPath([System.Drawing.Brushes]::White, $page)
  $fold = New-Object System.Drawing.Drawing2D.GraphicsPath
  $fold.AddLines([System.Drawing.PointF[]]@((pt ($px + $pw - $f) $py), (pt ($px + $pw - $f) ($py + $f)), (pt ($px + $pw) ($py + $f))))
  $fold.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush (rgb 0xB9 0xCF 0xF7)), $fold)

  # Text lines on bigger sizes.
  if ($s -ge 32) {
    $pen = New-Object System.Drawing.Pen (rgb 0x8F 0xB1 0xEE), ([float][Math]::Max(1.5, $s * 0.045))
    for ($i = 0; $i -lt 3; $i++) {
      $y = $py + $ph * (0.42 + $i * 0.17)
      $x1 = $px + $pw * $(if ($i -eq 2) { 0.6 } else { 0.8 })
      $g.DrawLine($pen, (pt ($px + $pw * 0.2) $y), (pt $x1 $y))
    }
  }
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  if ($s -ge 256) {
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  } else {
    # Classic DIB frame: BITMAPINFOHEADER (double height), BGRA rows bottom-up, 1-bit AND mask.
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $s, $s),
                          [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $raw = New-Object byte[] ($data.Stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($data)
    $w = New-Object System.IO.BinaryWriter $ms
    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2)); $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]0); $w.Write([UInt32]($s * $s * 4 + $maskStride * $s)); $w.Write([Int32]0); $w.Write([Int32]0)
    $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($y = $s - 1; $y -ge 0; $y--) { $w.Write($raw, $y * $data.Stride, $s * 4) }
    $w.Write((New-Object byte[] ($maskStride * $s)))
    $w.Flush()
  }
  $pngs += , $ms.ToArray()
  $bmp.Dispose()
}

# ICO container (PNG for 256 px, DIB for the rest).
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
  $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([UInt16]1); $bw.Write([UInt16]32)
  $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
Write-Output "written $Out"
