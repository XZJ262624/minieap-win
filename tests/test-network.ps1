param(
    [string]$Executable,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Interface,
    [string]$ExpectedDescription = 'Intel(R) Ethernet Controller I226-V'
)

# These checks enumerate/open the adapter only. They never start authentication.
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Executable) { $Executable = Join-Path $repoRoot 'x64/Debug/minieap-win.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$resultRoot = Join-Path $repoRoot 'port-validation/network-tests'
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
$emptyConfig = Join-Path $resultRoot 'empty.conf'
[IO.File]::WriteAllText($emptyConfig, "# Empty configuration for read-only integration checks.`r`n")
$results = [Collections.Generic.List[object]]::new()

function Invoke-NetworkCheck([string]$Name, [string[]]$CommandArguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $repoRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    # Quote argv using the Windows CRT convention, including trailing slashes.
    $info.Arguments = ($CommandArguments | ForEach-Object {
        '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "Could not start $Name" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "$Name did not finish within 15 seconds."
        }
        $output = $stdout.Result + $stderr.Result
        [IO.File]::WriteAllText((Join-Path $resultRoot "$Name.log"), $output, [Text.Encoding]::UTF8)
        return [pscustomobject]@{
            name = $Name
            arguments = $CommandArguments
            exitCode = $process.ExitCode
            output = $output
        }
    } finally {
        $process.Dispose()
    }
}

try {
    $listing = Invoke-NetworkCheck 'list-interfaces' @('--conf-file', $emptyConfig, '--list-interfaces')
    $results.Add($listing)
    if ($listing.exitCode -ne 0 -or -not $listing.output.Contains($Interface)) {
        throw 'Npcap enumeration did not include the requested interface.'
    }
    if ($ExpectedDescription -and -not $listing.output.Contains($ExpectedDescription)) {
        throw 'Windows adapter description was not present in Npcap enumeration.'
    }
    Write-Host 'PASS: Npcap GUID and Windows adapter description are listed.'

    $checkArguments = @('--conf-file', $emptyConfig, '--check-interface', '-n', $Interface,
        '--if-impl', 'libpcap', '--module', 'rjv3', '--service', 'students', '-b', '0')
    $check = Invoke-NetworkCheck 'check-interface' $checkArguments
    $results.Add($check)
    if ($check.exitCode -ne 0 -or -not $check.output.Contains('0x888e') -or
        -not $check.output.Contains($Interface)) {
        throw 'Npcap device opening, EAPOL filter setup, or RJv3 option parsing failed.'
    }
    Write-Host 'PASS: Intel I226-V opened; Ethernet/EAPOL filter and --module rjv3 --service students accepted.'

    $invalid = Invoke-NetworkCheck 'invalid-interface' @('--conf-file', $emptyConfig,
        '--check-interface', '-n', '\Device\NPF_{00000000-0000-0000-0000-000000000000}',
        '--if-impl', 'libpcap', '--module', 'rjv3', '--service', 'students', '-b', '0')
    $results.Add($invalid)
    if ($invalid.exitCode -eq 0 -or $invalid.exitCode -eq -1073741819 -or
        -not $invalid.output.Contains('libpcap')) {
        throw 'An invalid Npcap device was not rejected cleanly.'
    }
    Write-Host 'PASS: Invalid Npcap device rejected with a nonzero exit status.'
    Write-Host 'No authentication frames were sent. All checks use --list-interfaces or --check-interface.'
} finally {
    $report = [ordered]@{
        timestamp = [DateTimeOffset]::Now.ToString('o')
        executable = $Executable
        interface = $Interface
        authenticationPerformed = $false
        tests = @($results | ForEach-Object {
            [ordered]@{ name = $_.name; arguments = $_.arguments; exitCode = $_.exitCode }
        })
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $resultRoot 'results.json') -Encoding utf8
}
