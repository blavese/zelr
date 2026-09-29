# The WebP files userland/webptest.c decodes, and what Windows' own WebP
# decoder (WIC's "Microsoft Webp Decoder") makes of them. Writes
# userland/webpdata.h.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/genwebp.ps1 [-Edge <msedge.exe>]
#
# The pictures are this project's own, drawn pixel by pixel in
# tools/genwebp.html; the encoding is not: the page hands them to the
# browser's WebP encoder, and this script runs the page in headless Edge and
# reads the files back out of it. A decoder checked against its own encoder
# agrees with itself and proves nothing (tools/gengif.ps1 has Windows' GDI+
# encode the GIFs for the same reason), and a decoder checked against its own
# idea of the answer likewise, so the answers are Windows' decoder's.
#
# For a lossless file the answer is every pixel: a 32-bit FNV-1a hash of its
# red, green and blue laid over WEBP_REF_BG (the way webp.h lays a
# transparent pixel over a page), and of its alpha. For a lossy one, whose
# red, green and blue the two decoders round differently, it is samples,
# every `step` pixels across and down (3 or 5, neither sharing a factor with
# the 4, 8 and 16 pixel blocks), and the hash of the alpha, which is lossless
# even beside a lossy picture.
#
# The same Edge makes the same files, so the header comes out byte for byte
# the same; a newer Edge may encode differently, and then the header changes
# with it, answers and all. The Edge it was made with is named in the header.
param([string]$Edge)
Add-Type -AssemblyName PresentationCore
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bg = 0x336699

if (-not $Edge) {
    $look = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
              "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe")
    $look += Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft\EdgeCore\*\msedge.exe" -ErrorAction SilentlyContinue |
             Sort-Object FullName -Descending | ForEach-Object { $_.FullName }
    $Edge = $look | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $Edge) { throw "no msedge.exe here; give its path with -Edge" }

# Run the page. Edge writes the page's DOM to its standard output, which
# only reaches a file when it is redirected this way; a profile of its own
# keeps it off the user's.
$tmp = Join-Path $env:TEMP ("genwebp-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $tmp | Out-Null
try {
    $page = "file:///" + ((Join-Path $root "tools\genwebp.html") -replace '\\', '/')
    $p = Start-Process -FilePath $Edge -Wait -PassThru -NoNewWindow `
        -ArgumentList @("--headless=new", "--disable-gpu", "--no-first-run", "--disable-extensions",
                        "--user-data-dir=`"$tmp\profile`"", "--dump-dom", "`"$page`"") `
        -RedirectStandardOutput "$tmp\dom.txt" -RedirectStandardError "$tmp\err.txt"
    $dom = [IO.File]::ReadAllText("$tmp\dom.txt")
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
$encoder = [regex]::Match($dom, "encoder ([^\r\n<]+)").Groups[1].Value
$files = [regex]::Matches($dom, "(\w+) ([0-9.]+) data:image/webp;base64,([A-Za-z0-9+/=]+)")
if (-not $encoder -or $files.Count -eq 0) { throw "Edge gave no pictures (exit $($p.ExitCode))" }
$edgeVersion = (Get-Item $Edge).VersionInfo.ProductVersion

# The per-pixel work, compiled: PowerShell's own arithmetic has no 32-bit
# unsigned multiply that wraps, and is slow at a hundred thousand of them.
Add-Type -TypeDefinition @"
public static class WebpReference {
    public static uint Fnv(byte[] b) {
        uint h = 2166136261;
        foreach (byte x in b) { h ^= x; h *= 16777619; }
        return h;
    }
    // Bgra32 pixels as red, green, blue laid over (br, bg, bb), dividing as
    // webp.h does, whole numbers rounded down.
    public static byte[] Over(byte[] px, int n, int br, int bg, int bb) {
        byte[] o = new byte[n * 3];
        for (int i = 0; i < n; i++) {
            int b = px[i * 4], g = px[i * 4 + 1], r = px[i * 4 + 2], a = px[i * 4 + 3];
            if (a != 255) {
                r = (r * a + br * (255 - a)) / 255;
                g = (g * a + bg * (255 - a)) / 255;
                b = (b * a + bb * (255 - a)) / 255;
            }
            o[i * 3] = (byte)r; o[i * 3 + 1] = (byte)g; o[i * 3 + 2] = (byte)b;
        }
        return o;
    }
    public static byte[] Alpha(byte[] px, int n) {
        byte[] o = new byte[n];
        for (int i = 0; i < n; i++) o[i] = px[i * 4 + 3];
        return o;
    }
}
"@

function Decode([byte[]]$bytes) {
    $ms = New-Object System.IO.MemoryStream(, $bytes)
    $dec = [System.Windows.Media.Imaging.BitmapDecoder]::Create($ms,
        [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
        [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
    if ($dec.CodecInfo.FriendlyName -ne "Microsoft Webp Decoder") {
        throw "decoded by '$($dec.CodecInfo.FriendlyName)', not Windows' WebP decoder"
    }
    # Straight (not premultiplied) alpha, so nothing is lost before the
    # laying over below.
    $cv = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap($dec.Frames[0],
        [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
    $w = $cv.PixelWidth; $h = $cv.PixelHeight
    $px = New-Object byte[] ($w * $h * 4)
    $cv.CopyPixels($px, $w * 4, 0)
    return @{ W = $w; H = $h; Px = $px }
}

# The chunks after the RIFF header, and the ALPH filter if there is one.
function Chunks([byte[]]$b) {
    $names = @(); $at = 12; $filter = -1
    while ($at + 8 -le $b.Length) {
        $name = [Text.Encoding]::ASCII.GetString($b, $at, 4)
        $size = [BitConverter]::ToUInt32($b, $at + 4)
        if ($name -eq "ALPH") { $filter = ($b[$at + 8] -shr 2) -band 3 }
        $names += $name.Trim()
        $at += 8 + $size + ($size -band 1)
    }
    return @{ Names = $names; Filter = $filter }
}

function B64([byte[]]$bytes) {
    $s = [Convert]::ToBase64String($bytes)
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $s.Length; $i += 76) {
        [void]$sb.Append("    `"").Append($s.Substring($i, [Math]::Min(76, $s.Length - $i))).Append("`"`n")
    }
    if ($s.Length -eq 0) { [void]$sb.Append("    `"`"`n") }
    return $sb.ToString()
}

$br = ($bg -shr 16) -band 255; $bgg = ($bg -shr 8) -band 255; $bb = $bg -band 255
$body = ""
$table = ""
$total = 0
foreach ($m in $files) {
    $name = $m.Groups[1].Value; $q = $m.Groups[2].Value
    $bytes = [Convert]::FromBase64String($m.Groups[3].Value)
    $total += $bytes.Length
    $img = Decode $bytes
    $w = $img.W; $h = $img.H; $px = $img.Px
    $info = Chunks $bytes
    $lossless = $info.Names -contains "VP8L"
    $step = if ($w -ge 200) { 5 } else { 3 }

    # Every pixel laid over the background, and every alpha, in raster order.
    $over = [WebpReference]::Over($px, $w * $h, $br, $bgg, $bb)
    $alpha = [WebpReference]::Alpha($px, $w * $h)
    $samples = New-Object System.Collections.Generic.List[byte]
    if (-not $lossless) {
        for ($y = 0; $y -lt $h; $y += $step) {
            for ($x = 0; $x -lt $w; $x += $step) {
                $o = ($y * $w + $x) * 3
                $samples.Add($over[$o]); $samples.Add($over[$o + 1]); $samples.Add($over[$o + 2])
            }
        }
    }
    $id = "WEBP_" + $name.ToUpper() + $(if ($lossless) { "_LL" } else { "_Q" + [int]([double]$q * 100) })
    $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)).Replace("-", "").ToLower()
    $what = ($info.Names -join " ") + $(if ($info.Filter -ge 0) { ", ALPH filter $($info.Filter)" } else { "" })
    $body += "/* $name at quality $q`: ${w}x$h, $($bytes.Length) bytes ($what), sha256 $sha */`n"
    $body += "static const char ${id}_FILE[] =`n" + (B64 $bytes) + "    ;`n"
    if (-not $lossless) { $body += "static const char ${id}_RGB[] =`n" + (B64 $samples.ToArray()) + "    ;`n" }
    $body += "`n"
    $table += ("    {{ `"{0} {1}`", {2}_FILE, {3}, {4}, {5}, {6}, {7}, {8}, 0x{9:x8}u, 0x{10:x8}u }},`n" -f
               $name, $q, $id, $bytes.Length, $w, $h, [int]$lossless, $step,
               $(if ($lossless) { "0" } else { "${id}_RGB" }), [WebpReference]::Fnv($over), [WebpReference]::Fnv($alpha))
    Write-Host ("{0,-8} {1,-5} {2,4}x{3,-4} {4,6} bytes  {5}" -f $name, $q, $w, $h, $bytes.Length, $what)
}

$head = "/* Generated by tools/genwebp.ps1: the pictures are drawn by tools/genwebp.html`n" +
        "   and encoded by a browser's WebP encoder, the answers are what Windows' own`n" +
        "   WebP decoder (WIC's `"Microsoft Webp Decoder`") makes of the files. Not by`n" +
        "   hand, and not by anything in this project's decoder.`n" +
        "     encoder: msedge.exe $edgeVersion, $encoder`n" +
        "   For each file: a lossless one's every pixel, as FNV-1a hashes of its red,`n" +
        "   green and blue over WEBP_REF_BG, as (colour * alpha + bg * (255 - alpha))`n" +
        "   / 255, and of its alpha; a lossy one's samples, every step pixels across`n" +
        "   and down from the top left, red, green and blue over WEBP_REF_BG, and the`n" +
        "   hash of its alpha. */`n" +
        "#define WEBP_REF_BG " + ("0x{0:X6}" -f $bg) + "`n`n" +
        "typedef struct {`n" +
        "    const char *name;        /* the picture and the quality it was encoded at */`n" +
        "    const char *file;        /* base64 */`n" +
        "    int size, w, h;`n" +
        "    int lossless;            /* VP8L: every pixel is compared, by hash */`n" +
        "    int step;                /* lossy: samples every step pixels */`n" +
        "    const char *rgb;         /* lossy: the samples, base64 */`n" +
        "    unsigned int rgb_hash;   /* every pixel over WEBP_REF_BG */`n" +
        "    unsigned int alpha_hash; /* every alpha */`n" +
        "} webp_ref;`n`n"
$text = $head + $body + "static const webp_ref WEBP_REFS[] = {`n" + $table + "};`n" +
        "#define WEBP_REF_COUNT ((int)(sizeof WEBP_REFS / sizeof WEBP_REFS[0]))`n"
[System.IO.File]::WriteAllText((Join-Path $root "userland\webpdata.h"), $text, (New-Object System.Text.UTF8Encoding $false))
"wrote userland/webpdata.h: $($text.Length) bytes, the files $total bytes"
