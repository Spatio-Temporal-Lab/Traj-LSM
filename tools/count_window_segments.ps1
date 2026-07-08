$meta = Import-Csv 'D:\Project\data\td_wx\segments_meta.csv'
$windows = Import-Csv 'D:\Project\data\td_wx\windows6.csv'
$out = 'D:\Project\data\td_wx\windows6.csv'

# Build updated CSV with segment count column
$lines = @()
$header = (Get-Content $out -Head 1).TrimEnd()
$lines += "$header,STSegments"

foreach ($w in $windows) {
    $t_min = [long]$w.TMin; $t_max = [long]$w.TMax
    $x_min = [double]$w.XMin; $x_max = [double]$w.XMax
    $y_min = [double]$w.YMin; $y_max = [double]$w.YMax
    $count = 0
    foreach ($r in $meta) {
        $ts = [long]$r.t_start_unix; $te = [long]$r.t_end_unix
        $lo1 = [double]$r.lon_min; $lo2 = [double]$r.lon_max
        $la1 = [double]$r.lat_min; $la2 = [double]$r.lat_max
        if ($te -ge $t_min -and $ts -le $t_max -and
            $lo2 -ge $x_min -and $lo1 -le $x_max -and
            $la2 -ge $y_min -and $la1 -le $y_max) {
            $count++
        }
    }
    Write-Output "$($w.Label): $count segments"
    $lines += "$($w.Label),$($w.TMin),$($w.TMax),$($w.XMin),$($w.XMax),$($w.YMin),$($w.YMax),$count"
}

$lines -join "`n" | Out-File $out -Encoding ascii
Write-Output "Updated windows6.csv with STSegments column"
