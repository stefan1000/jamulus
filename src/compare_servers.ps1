# PowerShell script to compare Jamulus server lists
param (
    [string]$pivotserver = "MJTH|Blues/Rock"
)
# --- Country Code Conversion Helper ---
# The `$phpIdToCountryCode` variable is a comprehensive map from PHP ID to ISO 3166-1 alpha-2 country code.
# This map is manually generated and ensures a direct and reliable conversion between the numeric ID and the standardized two-letter country code.
# It is used to derive the country code for the explorer reference, which is essential for maintaining data consistency and accuracy.
# The map includes special cases and unofficial codes to handle the full range of data from the source.
#
# Map from PHP ID -> ISO 3166-1 alpha-2 Code

$phpIdToCountryCode = @{
    0 = ''; 1 = 'AF'; 2 = 'AL'; 3 = 'DZ'; 4 = 'AS'; 5 = 'AD'; 6 = 'AO'; 7 = 'AI'; 8 = 'AQ'; 9 = 'AG';
    10 = 'AR'; 11 = 'AM'; 12 = 'AW'; 13 = 'AU'; 14 = 'AT'; 15 = 'AZ'; 16 = 'BS'; 17 = 'BH'; 18 = 'BD'; 19 = 'BB';
    20 = 'BY'; 21 = 'BE'; 22 = 'BZ'; 23 = 'BJ'; 24 = 'BM'; 25 = 'BT'; 26 = 'BO'; 27 = 'BA'; 28 = 'BW'; 29 = 'BV';
    30 = 'BR'; 31 = 'IO'; 32 = 'BN'; 33 = 'BG'; 34 = 'BF'; 35 = 'BI'; 36 = 'KH'; 37 = 'CM'; 38 = 'CA'; 39 = 'CV';
    40 = 'KY'; 41 = 'CF'; 42 = 'TD'; 43 = 'CL'; 44 = 'CN'; 45 = 'CX'; 46 = 'CC'; 47 = 'CO'; 48 = 'KM'; 49 = 'CD';
    50 = 'CG'; 51 = 'CK'; 52 = 'CR'; 53 = 'CI'; 54 = 'HR'; 55 = 'CU'; 56 = 'CY'; 57 = 'CZ'; 58 = 'DK'; 59 = 'DJ';
    60 = 'DM'; 61 = 'DO'; 62 = 'TL'; 63 = 'EC'; 64 = 'EG'; 65 = 'SV'; 66 = 'GQ'; 67 = 'ER'; 68 = 'EE'; 69 = 'ET';
    70 = 'FK'; 71 = 'FO'; 72 = 'FJ'; 73 = 'FI'; 74 = 'FR'; 75 = 'GG'; 76 = 'GF'; 77 = 'PF'; 78 = 'TF'; 79 = 'GA';
    80 = 'GM'; 81 = 'GE'; 82 = 'DE'; 83 = 'GH'; 84 = 'GI'; 85 = 'GR'; 86 = 'GL'; 87 = 'GD'; 88 = 'GP'; 89 = 'GU';
    90 = 'GT'; 91 = 'GN'; 92 = 'GW'; 93 = 'GY'; 94 = 'HT'; 95 = 'HM'; 96 = 'HN'; 97 = 'HK'; 98 = 'HU'; 99 = 'IS';
    100 = 'IN'; 101 = 'ID'; 102 = 'IR'; 103 = 'IQ'; 104 = 'IE'; 105 = 'IL'; 106 = 'IT'; 107 = 'JM'; 108 = 'JP'; 109 = 'JO';
    110 = 'KZ'; 111 = 'KE'; 112 = 'KI'; 113 = 'KP'; 114 = 'KR'; 115 = 'KW'; 116 = 'KG'; 117 = 'LA'; 118 = 'LV'; 119 = 'LB';
    120 = 'LS'; 121 = 'LR'; 122 = 'LY'; 123 = 'LI'; 124 = 'LT'; 125 = 'LU'; 126 = 'MO'; 127 = 'MK'; 128 = 'MG'; 129 = 'MW';
    130 = 'MY'; 131 = 'MV'; 132 = 'ML'; 133 = 'MT'; 134 = 'MH'; 135 = 'MQ'; 136 = 'MR'; 137 = 'MU'; 138 = 'YT'; 139 = 'MX';
    140 = 'FM'; 141 = 'MD'; 142 = 'MC'; 143 = 'MN'; 144 = 'MS'; 145 = 'MA'; 146 = 'MZ'; 147 = 'MM'; 148 = 'NA'; 149 = 'NR';
    150 = 'NP'; 151 = 'NL'; 152 = 'CW'; 153 = 'NC'; 154 = 'NZ'; 155 = 'NI'; 156 = 'NE'; 157 = 'NG'; 158 = 'NU'; 159 = 'NF';
    160 = 'MP'; 161 = 'NO'; 162 = 'OM'; 163 = 'PK'; 164 = 'PW'; 165 = 'PS'; 166 = 'PA'; 167 = 'PG'; 168 = 'PY'; 169 = 'PE';
    170 = 'PH'; 171 = 'PN'; 172 = 'PL'; 173 = 'PT'; 174 = 'PR'; 175 = 'QA'; 176 = 'RE'; 177 = 'RO'; 178 = 'RU'; 179 = 'RW';
    180 = 'KN'; 181 = 'LC'; 182 = 'VC'; 183 = 'WS'; 184 = 'SM'; 185 = 'ST'; 186 = 'SA'; 187 = 'SN'; 188 = 'SC'; 189 = 'SL';
    190 = 'SG'; 191 = 'SK'; 192 = 'SI'; 193 = 'SB'; 194 = 'SO'; 195 = 'ZA'; 196 = 'GS'; 197 = 'ES'; 198 = 'LK'; 199 = 'SH';
    200 = 'PM'; 201 = 'SD'; 202 = 'SR'; 203 = 'SJ'; 204 = 'SZ'; 205 = 'SE'; 206 = 'CH'; 207 = 'SY'; 208 = 'TW'; 209 = 'TJ';
    210 = 'TZ'; 211 = 'TH'; 212 = 'TG'; 213 = 'TK'; 214 = 'TO'; 215 = 'TT'; 216 = 'TN'; 217 = 'TR'; 218 = 'TM'; 219 = 'TC';
    220 = 'TV'; 221 = 'UG'; 222 = 'UA'; 223 = 'AE'; 224 = 'GB'; 225 = 'US'; 226 = 'UM'; 227 = 'UY'; 228 = 'UZ'; 229 = 'VU';
    230 = 'VA'; 231 = 'VE'; 232 = 'VN'; 233 = 'VG'; 234 = 'VI'; 235 = 'WF'; 236 = 'EH'; 237 = 'YE'; 238 = 'IC'; 239 = 'ZM';
    240 = 'ZW'; 241 = 'CP'; 242 = 'ME'; 243 = 'RS'; 244 = 'BL'; 245 = 'MF'; 246 = ''; 247 = 'AC'; 248 = 'AX'; 249 = 'DG';
    250 = 'EA'; 251 = 'IM'; 252 = 'JE'; 253 = 'TA'; 254 = 'SS'; 255 = 'BQ'; 256 = 'SX'; 257 = 'XK'; 258 = ''; 259 = '';
    260 = ''; 261 = ''
}

# --- End Helper ---

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

                $propertiesToExclude = @('PSComputerName', 'RunspaceId', 'PSShowComputerName')
                $properties = $server.psobject.Properties.Name | Where-Object { $_ -notin $propertiesToExclude }

                foreach ($propName in $properties) {
                    $localValue = $server.$propName
                    $explorerValue = $explorerServer.$propName

                    # Special handling for country_code to derive it for explorer
                    if ($propName -eq 'country_code') {
                        $explorerValue = $phpIdToCountryCode[$explorerServer.countryid]
                    }
                    
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
                $cleanServer = $server | Select-Object -ExcludeProperty 'PSComputerName', 'RunspaceId', 'PSShowComputerName'
                Write-Host ($cleanServer | ConvertTo-Json -Depth 5)
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

