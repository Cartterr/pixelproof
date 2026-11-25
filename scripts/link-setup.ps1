# Temporary addressing for a DIRECT laptop<->Pi cable (Windows side). Run as administrator.
#   .\link-setup.ps1 -Adapter "Ethernet 3"            # adds 10.77.0.1/24, no gateway
#   .\link-setup.ps1 -Adapter "Ethernet 3" -Remove    # removes it again
# No default gateway and a high interface metric, so the Wi-Fi default route and Tailscale
# management path are untouched. Pair with link-setup-pi.sh on the Pi (10.77.0.2/24).
param(
  [Parameter(Mandatory = $true)][string]$Adapter,
  [string]$Ip = '10.77.0.1',
  [int]$Prefix = 24,
  [switch]$Remove
)
$ErrorActionPreference = 'Stop'
if ($Remove) {
  Get-NetIPAddress -InterfaceAlias $Adapter -IPAddress $Ip -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false
  Write-Host "removed $Ip from $Adapter"
  exit 0
}
$a = Get-NetAdapter -Name $Adapter
Write-Host ("{0}: {1}, {2}, link {3}" -f $a.Name, $a.InterfaceDescription, $a.Status, $a.LinkSpeed)
if (-not (Get-NetIPAddress -InterfaceAlias $Adapter -IPAddress $Ip -ErrorAction SilentlyContinue)) {
  New-NetIPAddress -InterfaceAlias $Adapter -IPAddress $Ip -PrefixLength $Prefix -SkipAsSource $false | Out-Null
}
Set-NetIPInterface -InterfaceAlias $Adapter -AddressFamily IPv4 -InterfaceMetric 9000
Get-NetIPAddress -InterfaceAlias $Adapter -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress, PrefixLength, PrefixOrigin -AutoSize
Get-NetRoute -AddressFamily IPv4 -DestinationPrefix 0.0.0.0/0 | Format-Table InterfaceAlias, NextHop, RouteMetric -AutoSize
