# build.ps1 - One-shot build for this project.
# Activates the ESP-IDF environment (via idf_env.ps1) and runs idf.py.
#
# USAGE:
#   .\scripts\build.ps1              # => idf.py build
#   .\scripts\build.ps1 flash monitor
#   .\scripts\build.ps1 fullclean
#   .\scripts\build.ps1 --encrypt    # => build, then produce the encrypted OTA artifact
#
# Any arguments are passed straight through to idf.py, EXCEPT --encrypt, which is
# consumed by this script (see below). With no args, builds.
#
# PRE-ENCRYPTED OTA - RELEASE STEP (opt-in via --encrypt):
#   The normal build produces the PLAINTEXT build\modulo_wifi.bin, which is what
#   beta testers flash over UART. For the OTA path, that same .bin is encrypted
#   with the RSA PUBLIC key to produce build\modulo_wifi_enc.bin, which is what
#   gets uploaded to S3 (the manifest "url" points at it).
#
#   Pass --encrypt to run that encrypt step automatically after a successful
#   build. It is OPT-IN (not wired into every build) on purpose: most dev builds
#   never go to S3, the plaintext .bin is always needed for UART regardless, and
#   coupling plain compilation to the release key would make every checkout need
#   the public key. --encrypt is guarded on the public key being present; if it's
#   missing, the build still succeeds and only the encrypt step is skipped (with a
#   warning). The encrypted artifact is an ADDITIONAL output: the plaintext .bin
#   is always produced too.

$ErrorActionPreference = "Continue"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

. (Join-Path $scriptDir "idf_env.ps1")

# Consume --encrypt (case-insensitive) before passing args to idf.py
$doEncrypt = $false
$idfArgs = @()
foreach ($a in $args) {
    if ($a -ieq "--encrypt") {
        $doEncrypt = $true
    } else {
        $idfArgs += $a
    }
}

# Run the normal build / idf.py command
if ($idfArgs.Count -gt 0) {
    & idf.py @idfArgs
} else {
    & idf.py build
}
$buildExit = $LASTEXITCODE

# Opt-in encrypted OTA artifact
if ($doEncrypt) {
    if ($buildExit -ne 0) {
        Write-Warning "Build failed (exit $buildExit) - skipping --encrypt step."
        exit $buildExit
    }

    $tool    = Join-Path $repoRoot "managed_components\espressif__esp_encrypted_img\tools\esp_enc_img_gen.py"
    $pubKey  = Join-Path $repoRoot "main\ota_decrypt_pub.pem"
    $plain   = Join-Path $repoRoot "build\modulo_wifi.bin"
    $encOut  = Join-Path $repoRoot "build\modulo_wifi_enc.bin"

    if (-not (Test-Path $pubKey)) {
        Write-Warning "--encrypt: public key not found at main\ota_decrypt_pub.pem - skipping encryption. (Build succeeded; plaintext build\modulo_wifi.bin is ready.)"
        exit 0
    }
    if (-not (Test-Path $plain)) {
        Write-Warning "--encrypt: plaintext build\modulo_wifi.bin not found - nothing to encrypt."
        exit 0
    }

    Write-Host "Encrypting OTA artifact: build\modulo_wifi_enc.bin"
    & python $tool encrypt $plain $pubKey $encOut
    $encExit = $LASTEXITCODE
    if ($encExit -ne 0) {
        Write-Warning "--encrypt: encryption step failed (exit $encExit)."
        exit $encExit
    }
    Write-Host "Encrypted OTA artifact ready: build\modulo_wifi_enc.bin (upload to S3, point manifest url at it)."
    exit 0
}

exit $buildExit
