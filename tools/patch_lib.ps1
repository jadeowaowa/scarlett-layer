$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-Utf8([string]$Path) {
    [System.IO.File]::ReadAllText($Path, $script:Utf8NoBom)
}

function Write-Utf8([string]$Path, [string]$Content) {
    [System.IO.File]::WriteAllText($Path, $Content, $script:Utf8NoBom)
}

function Replace-Exact([string]$Text, [string]$Old, [string]$New, [string]$Label = "patch") {
    $nl = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $normOld = $Old.Replace("`r`n", "`n").Replace("`n", $nl)
    $normNew = $New.Replace("`r`n", "`n").Replace("`n", $nl)
    $count = ([regex]::Matches($Text, [regex]::Escape($normOld))).Count
    if ($count -ne 1) {
        throw "Replace-Exact failed for [$Label]: expected 1 match, found $count."
    }
    return $Text.Replace($normOld, $normNew)
}