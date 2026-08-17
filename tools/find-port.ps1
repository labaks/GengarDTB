# Resolves the board's serial port by device identity rather than a fixed number.
#
# Windows reassigns COM numbers whenever the board moves to another USB socket:
# this board has already been COM13 and COM4. Anything with the port baked in
# breaks silently the first time the cable is replugged elsewhere.
function Get-CydPort {
    $dev = Get-CimInstance Win32_PnPEntity |
           Where-Object { $_.Name -match 'USB-SERIAL CH340 \(COM\d+\)' } |
           Select-Object -First 1
    if (-not $dev) { throw "CH340 board not found on any COM port" }
    if ($dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    throw "cannot parse port from '$($dev.Name)'"
}
