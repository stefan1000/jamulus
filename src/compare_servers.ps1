# PowerShell script to compare Jamulus server lists
param (
    [string]$pivotserver = "Blues|Blues/Rock"
)

# Define URLs for the server lists
$jamstatusUrl = "http://localhost:3000/jamstatus"
$explorerUrl = "https://explorer.jamulus.io/servers.php?directory=anygenre1.jamulus.io:22124"

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

if (-not [string]::IsNullOrWhitespace($pivotserver)) {
    $foundServers = $jamstatusServers | Where-Object { $_.name -match $pivotserver }
    if ($foundServers) {
        Write-Host "`n--- Pivot Server Details ---"
        foreach ($server in $foundServers) {
            Write-Host "`n-- Comparing server: $($server.name) --"
            $explorerServer = $explorerServers | Where-Object { $_.ip -eq $server.ip -and $_.port -eq $server.port }

            if ($explorerServer) {
                # Column widths
                $keyColWidth = 20
                $valColWidth = 50

                # Headers
                $header = ("{0,-$keyColWidth}{1,-$valColWidth}{2,-$valColWidth}" -f "Field", "Local Jamstatus", "Explorer")
                Write-Host $header
                Write-Host ("-" * ($keyColWidth + ($valColWidth * 2)))

                $properties = $server.psobject.Properties.Name

                foreach ($propName in $properties) {
                    $localValue = $server.$propName
                    $explorerValue = $explorerServer.$propName
                    
                    # Convert values to comparable strings
                    $localValueStr = if ($null -eq $localValue) { "(null)" } elseif ($localValue -is [System.Collections.IEnumerable] -and -not ($localValue -is [string])) { $localValue | ConvertTo-Json -Compress } else { $localValue.ToString() }
                    $explorerValueStr = if ($null -eq $explorerValue) { "(null)" } elseif ($explorerValue -is [System.Collections.IEnumerable] -and -not ($explorerValue -is [string])) { $explorerValue | ConvertTo-Json -Compress } else { $explorerValue.ToString() }

                    # Truncate long strings for display
                    $localDisplayStr = if ($localValueStr.Length -gt $valColWidth - 2) { $localValueStr.Substring(0, $valColWidth - 5) + "..." } else { $localValueStr }
                    $explorerDisplayStr = if ($explorerValueStr.Length -gt $valColWidth - 2) { $explorerValueStr.Substring(0, $valColWidth - 5) + "..." } else { $explorerValueStr }

                    $line = ("{0,-$keyColWidth}{1,-$valColWidth}{2,-$valColWidth}" -f $propName, $localDisplayStr, $explorerDisplayStr)

                    if ($localValueStr -ne $explorerValueStr) {
                        Write-Host $line -ForegroundColor Yellow
                    } else {
                        Write-Host $line
                    }
                }
            } else {
                Write-Warning "Server '$($server.name)' not found in Explorer list."
                Write-Host "`n- Local Jamstatus Details:"
                Write-Host ($server | ConvertTo-Json -Depth 5)
            }
        }
    }
    else {
        Write-Warning "Pivot server matching '$pivotserver' not found in Local Jamstatus list."
    }
}

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

    # Write-Host "`nLocal Jamstatus Unique IPs: $($jamstatusIps -join ', ')"
    # Write-Host "Explorer Unique IPs: $($explorerIps -join ', ')"

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
