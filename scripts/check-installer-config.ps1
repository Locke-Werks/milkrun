<#
.SYNOPSIS
Validates the token names in installer.toml.

.DESCRIPTION
Forge expands three separate sets of tokens, at different moments, and they are
not interchangeable. A token used outside its set is left in the string verbatim
rather than reported, so the failure surfaces on a user's machine as a shortcut
pointing at a literal "C:\{app}\..." or an install directory that turned out to
be a relative path.

That is exactly what shipped in 0.2.0. This runs in CI so it cannot ship again.

Sets are from Forge docs/config-schema.md, "Tokens".
#>
[CmdletBinding()]
param(
    [string]$Config
)

$ErrorActionPreference = 'Stop'

# Resolved here rather than as a parameter default: Windows PowerShell has not
# populated $PSScriptRoot by the time param defaults are evaluated.
if (-not $Config) {
    $Config = Join-Path (Split-Path -Parent $PSScriptRoot) 'installer\installer.toml'
}

# Valid only in install.dir.
$InstallDirTokens = @('ProgramFiles', 'ProgramData', 'LocalAppData', 'LocalPrograms', 'Desktop')

# Valid in registry key/value, env value, service binary/args, assoc
# open_command/icon, and hook args.
$DeclarationTokens = @('InstallDir', 'Product', 'Version')

# Valid additionally, and only, in hook args.
$HookArgTokens = @('UserProfile', 'UserAppData', 'UserLocalAppData', 'UserDesktop', 'UserPrograms')

if (-not (Test-Path $Config)) { throw "No config at $Config" }
$lines = Get-Content $Config

$problems = @()
$lineNo = 0
$inHookArgs = $false

foreach ($line in $lines) {
    $lineNo++

    # Strip comments so a token mentioned in prose is not flagged.
    $code = $line -replace '(?<!\\)#.*$', ''
    if ($code -notmatch '\{') { continue }

    $key = if ($code -match '^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*=') { $Matches[1] } else { '' }
    if ($code -match '^\s*\[\[hooks\.') { $inHookArgs = $true }
    elseif ($code -match '^\s*\[') { $inHookArgs = $false }

    # A shortcut target is the documented exception: it expands InstallDir and
    # nothing else, not even the other declaration tokens.
    $isShortcutTarget = ($key -eq 'target')

    $allowed =
        if ($key -eq 'dir')            { $InstallDirTokens }
        elseif ($isShortcutTarget)     { @('InstallDir') }
        elseif ($inHookArgs)           { $DeclarationTokens + $HookArgTokens }
        else                           { $DeclarationTokens }

    foreach ($m in [regex]::Matches($code, '\{([^}]*)\}')) {
        $tok = $m.Groups[1].Value
        if ($allowed -contains $tok) { continue }

        $where =
            if ($key -eq 'dir')        { 'install.dir' }
            elseif ($isShortcutTarget) { 'a shortcut target' }
            elseif ($inHookArgs)       { 'hook args' }
            else                       { 'a declaration' }

        $problems += [pscustomobject]@{
            Line    = $lineNo
            Token   = "{$tok}"
            Where   = $where
            Allowed = ($allowed | ForEach-Object { "{$_}" }) -join ', '
            Text    = $line.Trim()
        }
    }
}

if ($problems.Count -eq 0) {
    Write-Host "installer.toml: all tokens valid for their context."
    exit 0
}

Write-Host "installer.toml has tokens Forge will not expand:`n"
foreach ($p in $problems) {
    Write-Host ("  line {0}: {1} is not valid in {2}" -f $p.Line, $p.Token, $p.Where)
    Write-Host ("    {0}" -f $p.Text)
    Write-Host ("    valid here: {0}`n" -f $p.Allowed)
}
Write-Host "Forge leaves an unknown token in the string rather than failing, so this"
Write-Host "would install to the wrong place instead of erroring."
exit 1
