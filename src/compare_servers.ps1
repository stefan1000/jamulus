# PowerShell script to compare Jamulus server lists

# Define URLs for the server lists
$jamstatusUrl = "http://localhost:3000/jamstatus"
$explorerUrl = "https://explorer.jamulus.io/servers.php?directory=jazz.jamulus.io:22324"

function Get-ServerList {
    param (
        [string]$Url,
        [string]$SourceName
    )
    Write-Host "Fetching $SourceName server list from $Url..."
    try {
        # Invoke-RestMethod automatically parses JSON response
        $response = Invoke-RestMethod -Uri $Url -TimeoutSec 30
        Write-Host "Successfully fetched $SourceName server list."
        return $response
    }
    catch {
            Write-Error "Failed to fetch $SourceName server list from ${Url}: $($_.Exception.Message)"
        return $null
    }
}

# Fetch data concurrently using jobs
Write-Host "Starting concurrent fetches..."

$jamstatusJob = Start-Job -ScriptBlock {
    param($url, $sourceName)
    # The function needs to be defined within the scriptblock for Start-Job
    function Get-ServerList {
        param (
            [string]$Url,
            [string]$SourceName
        )
        Write-Host "Fetching $SourceName server list from $Url..."
        try {
            $response = Invoke-RestMethod -Uri $Url -TimeoutSec 30
            Write-Host "Successfully fetched $SourceName server list."
            return $response
        }
        catch {
                Write-Error "Failed to fetch $SourceName server list from ${Url}: $($_.Exception.Message)"
            return $null
        }
    }
    Get-ServerList -Url $url -SourceName $sourceName
} -ArgumentList $jamstatusUrl, "Local Jamstatus"

$explorerJob = Start-Job -ScriptBlock {
    param($url, $sourceName)
    # The function needs to be defined within the scriptblock for Start-Job
    function Get-ServerList {
        param (
            [string]$Url,
            [string]$SourceName
        )
        Write-Host "Fetching $SourceName server list from $Url..."
        try {
            $response = Invoke-RestMethod -Uri $Url -TimeoutSec 30
            Write-Host "Successfully fetched $SourceName server list."
            return $response
        }
        catch {
            Write-Error "Failed to fetch $SourceName server list from ${Url}: $($_.Exception.Message)"
            return $null
        }
    }
    Get-ServerList -Url $url -SourceName $sourceName
} -ArgumentList $explorerUrl, "Explorer"

# Wait for both jobs to complete and retrieve their results
Write-Host "Waiting for jobs to complete..."
Wait-Job -Job $jamstatusJob, $explorerJob | Out-Null

$jamstatusServers = Receive-Job -Job $jamstatusJob
$explorerServers = Receive-Job -Job $explorerJob

# Display job output (if any errors or Write-Host were used within job)
foreach ($job in @($jamstatusJob, $explorerJob)) {
    if ($job -and $job.HasErrors) {
        $errors = $job | Receive-Job -Keep -ErrorAction SilentlyContinue
        Write-Error "Error in job $($job.Id): $errors"
    }
}


# Clean up jobs
Remove-Job -Job $jamstatusJob, $explorerJob -Force | Out-Null

Write-Host "`n--- Consistency Check Results ---"

# Perform consistency checks
if ($jamstatusServers -eq $null -or $explorerServers -eq $null) {
    Write-Warning "Consistency check aborted: one or both server lists could not be fetched."
}
else {
    # 1. Compare number of servers
    $jamstatusCount = $jamstatusServers.Count
    $explorerCount = $explorerServers.Count

    Write-Host "Local Jamstatus Server Count: $jamstatusCount"
    Write-Host "Explorer Server Count: $explorerCount"

    if ($jamstatusCount -eq $explorerCount) {
        Write-Host "Server count: CONSISTENT" -ForegroundColor Green
    }
    else {
        Write-Warning "Server count: INCONSISTENT (Local: $jamstatusCount, Explorer: $explorerCount)"
    }

    # 2. Compare server IPs
    # Select unique IPs from both lists
    # Note: Explorer data has an 'ip' field, Jamstatus data also has an 'ip' field.
    # We select the 'ip' property as a string for direct comparison.
    $jamstatusIps = $jamstatusServers | Select-Object -ExpandProperty ip -Unique | Sort-Object
    $explorerIps = $explorerServers | Select-Object -ExpandProperty ip -Unique | Sort-Object

    Write-Host "`nLocal Jamstatus Unique IPs: $($jamstatusIps -join ', ')"
    Write-Host "Explorer Unique IPs: $($explorerIps -join ', ')"

    # Use Compare-Object to find differences
    $ipDifferences = Compare-Object $jamstatusIps $explorerIps -IncludeEqual

    # Filter out common IPs (where SideIndicator is '==')
    $mismatchedIps = $ipDifferences | Where-Object { $_.SideIndicator -ne '==' }

    if (-not $mismatchedIps) {
        Write-Host "Server IPs: CONSISTENT" -ForegroundColor Green
    }
    else {
        Write-Warning "Server IPs: INCONSISTENT"
        Write-Warning "Mismatch details (<= is Local Jamstatus, => is Explorer):"
        $mismatchedIps | Format-Table -AutoSize
    }
}

Write-Host "`nConsistency check finished."
