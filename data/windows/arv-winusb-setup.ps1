<#
.SYNOPSIS
Lists the USB3 Vision cameras and their drivers, and installs or removes
aravis-u3v.inf, which makes WinUSB their driver so that Aravis can open them.

.DESCRIPTION
On Windows, Aravis opens USB3 Vision cameras through libusb, which can only open
a camera whose driver is WinUSB, the recommended one, or libusbK or libusb0,
which libusb drives through libusbK.dll. aravis-u3v.inf installs WinUSB on the
USB3 Vision function of the cameras that have no better-ranked driver. It never
replaces the driver of a camera vendor's software.

Actions:

list       Lists the USB3 Vision functions of the connected cameras, with their
           driver, and says whether Aravis can open them. This is the default.
           Needs Windows 11.
install    Adds aravis-u3v.inf to the driver store and installs it on the
           cameras that it matches:
               pnputil /add-driver aravis-u3v.inf /install
           Needs administrator rights, and the signed catalog of the INF,
           aravis-u3v.cat, next to it.
uninstall  Removes aravis-u3v.inf from the driver store and from the cameras
           that use it, which go back to their next best driver, if any:
               pnputil /delete-driver oemNN.inf /uninstall
           Needs administrator rights.

With -WhatIf, install and uninstall print the pnputil commands instead of
running them, and need no administrator rights.

Windows installs only signed driver packages, and aravis-u3v.inf ships without
its catalog. In the folder of the INF, create the catalog with MakeCat and sign
it with SignTool, both from the Windows SDK:

    makecat aravis-u3v.cdf
    signtool sign /fd SHA256 /sha1 <certificate thumbprint> aravis-u3v.cat

The signing certificate must be in the Trusted Root Certification Authorities
and Trusted Publishers stores of the local machine on each machine where the
driver package is installed. The Windows section of the Aravis USB
documentation explains how to create such a certificate.

.PARAMETER Action
list (the default), install or uninstall.

.PARAMETER InfPath
The driver package. Default: aravis-u3v.inf, next to this script.

.PARAMETER DeviceListFile
For list: reads a saved output of
    pnputil /enum-devices /connected /deviceids /services /drivers [/format xml]
instead of running pnputil, for example one from another machine.

.PARAMETER DriverListFile
For uninstall with -WhatIf: reads a saved output of
    pnputil /enum-drivers [/format xml]
instead of running pnputil.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File arv-winusb-setup.ps1

Lists the connected USB3 Vision cameras and their drivers.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File arv-winusb-setup.ps1 install -WhatIf

Prints the command that installs aravis-u3v.inf.

.LINK
https://aravisproject.github.io/aravis/aravis-main/usb.html
#>

# Placed before the help, #Requires would hide the help from Get-Help
#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess = $true)]
param (
    [Parameter(Position = 0)]
    [ValidateSet('list', 'install', 'uninstall')]
    [string] $Action = 'list',

    [string] $InfPath,

    [string] $DeviceListFile,

    [string] $DriverListFile
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# USB3 Vision interfaces have the class EF and the subclass 05 (Aravis's
# src/arvuvinterfaceprivate.h). Windows gives the node of the USB3 Vision function
# of a camera the compatible IDs USB\Class_EF&SubClass_05&Prot_pp,
# USB\Class_EF&SubClass_05 and USB\Class_EF, and aravis-u3v.inf matches the first
# one with the protocol 00.
$UsbVisionId = 'USB\Class_EF&SubClass_05'
$InfMatchId = 'USB\Class_EF&SubClass_05&Prot_00'
$DocumentationUrl = 'https://aravisproject.github.io/aravis/aravis-main/usb.html'
# Besides WinUSB, the Windows backend of libusb drives the devices of these two
# drivers, which Zadig can install, through libusbK.dll (libusb's
# os/windows_winusb.c: winusbx_driver_names and winusbx_init)
$LibusbKDrivers = @('libusbK', 'libusb0')

function Get-SystemDirectory {
    # The System32 folder of 64-bit programs: a 32-bit PowerShell reaches it through Sysnative
    $directory = 'System32'
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        $directory = 'Sysnative'
    }
    return Join-Path $env:SystemRoot $directory
}

function Get-PnputilPath {
    # pnputil.exe has no 32-bit build
    return Join-Path (Get-SystemDirectory) 'pnputil.exe'
}

function Test-LibusbKDll ([string] $Directory = (Get-SystemDirectory)) {
    # libusb loads libusbK.dll from the system folder of Windows only
    # (load_system_library in its os/windows_common.c)
    return Test-Path -LiteralPath (Join-Path $Directory 'libusbK.dll') -PathType Leaf
}

function Format-Command ([string[]] $Arguments) {
    # The command line as it would be typed, quoting what cmd.exe or PowerShell would split
    $words = foreach ($argument in $Arguments) {
        if ($argument -match '^[A-Za-z0-9_.:\\/-]+$') { $argument } else { '"' + $argument + '"' }
    }
    return 'pnputil ' + ($words -join ' ')
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal $identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-PnputilXml ([string[]] $Arguments) {
    # Returns the XML output of a pnputil enumeration, or $null when this pnputil
    # lacks one of the options, as older versions of Windows 11 lack /format
    $file = [IO.Path]::GetTempFileName()
    try {
        $null = & (Get-PnputilPath) @Arguments '/format' 'xml' '/output-file' $file
        if ($LASTEXITCODE -eq 0) {
            $text = [IO.File]::ReadAllText($file)
            if ($text.TrimStart().StartsWith('<')) {
                return $text
            }
        }
    } finally {
        [IO.File]::Delete($file)
    }
    return $null
}

function Get-PnputilText ([string[]] $Arguments, [string] $Hint = '') {
    $lines = @(& (Get-PnputilPath) @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw ('{0} failed with exit code {1}.{2}{3}{4}' -f (Format-Command $Arguments), $LASTEXITCODE, $Hint,
               [Environment]::NewLine, ($lines -join [Environment]::NewLine))
    }
    return $lines
}

function Read-ListFile ([string] $Path) {
    return [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).ProviderPath)
}

function Test-Xml ([string] $Text) {
    return $Text.TrimStart().StartsWith('<')
}

function Repair-PnputilXml ([string] $Text) {
    # pnputil 10.0.26200 leaves out the start tag of the root element of an
    # /output-file when that tag, which repeats the command line with the path
    # of the file, would be 255 to 289 characters long, but writes its end tag
    $prolog = [regex]::Match($Text, '^\s*(<\?xml[^>]*\?>)?').Value
    $body = $Text.Substring($prolog.Length)
    if ($body -match '^\s*<PnpUtil[\s/>]' -or $body -notmatch '</PnpUtil>\s*$') {
        return $Text
    }
    return $prolog + '<PnpUtil>' + $body
}

function Test-UnreadText ([object[]] $Records, [string[]] $Lines) {
    # $true when the text output of pnputil holds records but none was read:
    # the text is in the language of Windows, and only English is understood
    return @($Records).Count -eq 0 -and @($Lines | Where-Object { $_ -match '\S' }).Count -gt 3
}

function Get-ChildText ($Node, [string] $Name) {
    $child = $Node.SelectSingleNode($Name)
    if ($null -eq $child) {
        return ''
    }
    return $child.InnerText.Trim()
}

function ConvertFrom-PnputilText ([string[]] $Lines, [string] $RecordLabel) {
    # Parses the text output of pnputil, which is in the language of Windows: only
    # English is understood. Each record is a hashtable mapping a label to its
    # values; '#Drivers' holds the nested records of the 'Matching Drivers' list.
    $records = New-Object System.Collections.ArrayList
    $record = $null
    $nested = $null
    $values = $null
    foreach ($line in $Lines) {
        if ($line -match '^( *)([^ :][^:]*?): *(.*?) *$' -and $Matches[1].Length -lt 8) {
            $indent = $Matches[1].Length
            $label = $Matches[2]
            $value = $Matches[3]
            if ($indent -eq 0 -and $label -eq $RecordLabel) {
                $record = @{ '#Drivers' = (New-Object System.Collections.ArrayList) }
                [void] $records.Add($record)
            }
            if ($null -eq $record) {
                continue
            }
            if ($indent -eq 0) {
                $nested = $null
                $target = $record
            } else {
                if ($null -eq $nested -or $label -eq 'Driver Name') {
                    $nested = @{}
                    [void] $record['#Drivers'].Add($nested)
                }
                $target = $nested
            }
            $values = New-Object System.Collections.ArrayList
            if ($value) {
                [void] $values.Add($value)
            }
            $target[$label] = $values
        } elseif ($line -match '^ {8,}(\S.*?) *$' -and $null -ne $values) {
            # Further values of a list, such as the hardware IDs
            [void] $values.Add($Matches[1])
        }
    }
    return , $records
}

function Get-TextValue ($Record, [string] $Label) {
    if ($Record.ContainsKey($Label) -and $Record[$Label].Count -gt 0) {
        return [string] $Record[$Label][0]
    }
    return ''
}

function Get-TextValues ($Record, [string] $Label) {
    if ($Record.ContainsKey($Label)) {
        return , @($Record[$Label])
    }
    return , @()
}

function New-Device ([string] $InstanceId, [string] $Description, [string] $Status, [string[]] $Problems,
                     [string] $DriverName, [string] $Service, [string[]] $HardwareIds, [string[]] $CompatibleIds,
                     [object[]] $Drivers) {
    return [pscustomobject] @{
        InstanceId = $InstanceId
        Description = $Description
        Status = $Status
        Problems = @($Problems | Where-Object { $_ })
        DriverName = $DriverName
        Service = $Service
        HardwareIds = @($HardwareIds | Where-Object { $_ })
        CompatibleIds = @($CompatibleIds | Where-Object { $_ })
        Drivers = @($Drivers | Where-Object { $_ })
    }
}

function New-Driver ([string] $DriverName, [string] $OriginalName, [string] $ProviderName, [string] $Status) {
    return [pscustomobject] @{
        DriverName = $DriverName
        OriginalName = $OriginalName
        ProviderName = $ProviderName
        Status = $Status
    }
}

function ConvertFrom-DeviceXml ([string] $Text) {
    $document = New-Object System.Xml.XmlDocument
    $document.LoadXml((Repair-PnputilXml $Text))
    $devices = foreach ($node in $document.SelectNodes('/PnpUtil/Device')) {
        $drivers = foreach ($driver in $node.SelectNodes('MatchingDrivers/DriverName')) {
            New-Driver $driver.GetAttribute('DriverName') (Get-ChildText $driver 'OriginalName') `
                       (Get-ChildText $driver 'ProviderName') (Get-ChildText $driver 'Status')
        }
        $problems = foreach ($child in $node.SelectNodes("*[starts-with(local-name(), 'Problem')]")) {
            '{0}: {1}' -f $child.LocalName, $child.InnerText.Trim()
        }
        $hardwareIds = @($node.SelectNodes('HardwareIds/HardwareId') | ForEach-Object { $_.InnerText.Trim() })
        $compatibleIds = @($node.SelectNodes('CompatibleIds/CompatibleId') | ForEach-Object { $_.InnerText.Trim() })
        New-Device -InstanceId $node.GetAttribute('InstanceId') -Description (Get-ChildText $node 'DeviceDescription') `
                   -Status (Get-ChildText $node 'Status') -Problems $problems `
                   -DriverName (Get-ChildText $node 'DriverName') -Service (Get-ChildText $node 'Service') `
                   -HardwareIds $hardwareIds -CompatibleIds $compatibleIds -Drivers $drivers
    }
    return , @($devices)
}

function ConvertFrom-DeviceText ([string[]] $Lines) {
    $records = ConvertFrom-PnputilText $Lines 'Instance ID'
    $devices = foreach ($record in $records) {
        $drivers = foreach ($driver in $record['#Drivers']) {
            New-Driver (Get-TextValue $driver 'Driver Name') (Get-TextValue $driver 'Original Name') `
                       (Get-TextValue $driver 'Provider Name') (Get-TextValue $driver 'Driver Status')
        }
        $problems = foreach ($label in @($record.Keys | Where-Object { $_ -like 'Problem*' } | Sort-Object)) {
            '{0}: {1}' -f $label, (Get-TextValue $record $label)
        }
        New-Device -InstanceId (Get-TextValue $record 'Instance ID') `
                   -Description (Get-TextValue $record 'Device Description') `
                   -Status (Get-TextValue $record 'Status') -Problems $problems `
                   -DriverName (Get-TextValue $record 'Driver Name') -Service (Get-TextValue $record 'Service') `
                   -HardwareIds (Get-TextValues $record 'Hardware IDs') `
                   -CompatibleIds (Get-TextValues $record 'Compatible IDs') -Drivers $drivers
    }
    return , @($devices)
}

function Get-UsbVisionDevices {
    # Returns the source of the list and the USB3 Vision function nodes
    $lines = $null
    if ($DeviceListFile) {
        $text = Read-ListFile $DeviceListFile
        $source = $DeviceListFile
        if (Test-Xml $text) {
            $devices = ConvertFrom-DeviceXml $text
        } else {
            $lines = $text -split '\r?\n'
            $devices = ConvertFrom-DeviceText $lines
        }
    } else {
        $arguments = @('/enum-devices', '/connected', '/deviceid', $UsbVisionId, '/deviceids', '/services', '/drivers')
        $devices = $null
        $text = Get-PnputilXml $arguments
        if ($null -ne $text) {
            $source = Format-Command ($arguments + @('/format', 'xml'))
            try {
                $devices = ConvertFrom-DeviceXml $text
            } catch {
                Write-Warning ('Could not read the XML output of {0}: {1}' -f $source, $_.Exception.Message)
            }
        }
        if ($null -eq $devices) {
            # Windows 11 before 22H2 has no /deviceid filter, and older versions
            # no /format option: list every device and filter here
            $arguments = @('/enum-devices', '/connected', '/deviceids', '/services', '/drivers')
            $lines = Get-PnputilText $arguments ' Its /deviceids and /services options need Windows 11.'
            $devices = ConvertFrom-DeviceText $lines
            $source = Format-Command $arguments
        }
    }
    if ($null -ne $lines -and (Test-UnreadText $devices $lines)) {
        $what = $source
        if (-not $DeviceListFile) {
            $what = 'the output of ' + $source
        }
        Write-Warning (('Could not read {0}, which is probably not in English: look in it for the devices ' -f $what) +
                       ('whose compatible IDs include {0}.' -f $UsbVisionId))
    }
    $matching = @($devices | Where-Object { $_.CompatibleIds -contains $UsbVisionId })
    return [pscustomobject] @{ Source = $source; Devices = $matching }
}

function Test-RegistryValue ([string] $Key, [string[]] $Names) {
    # $true when the registry key has one of the values, $false when it has none,
    # $null when the key cannot be read
    try {
        $item = Get-Item -LiteralPath $Key -ErrorAction Stop
    } catch {
        return $null
    }
    foreach ($name in $Names) {
        if ($null -ne $item.GetValue($name)) {
            return $true
        }
    }
    return $false
}

function Test-InterfaceGuid ([string] $InstanceId) {
    # libusb finds a WinUSB device through the device interface classes listed in
    # the DeviceInterfaceGUIDs, or DeviceInterfaceGUID, value of its Device
    # Parameters registry key
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $InstanceId + '\Device Parameters'
    return Test-RegistryValue $key @('DeviceInterfaceGUIDs', 'DeviceInterfaceGUID')
}

function Format-Package ($Device) {
    $installed = @($Device.Drivers | Where-Object { $_.DriverName -eq $Device.DriverName }) | Select-Object -First 1
    if ($null -ne $installed -and $installed.OriginalName) {
        return '{0} ({1}, {2})' -f $Device.DriverName, $installed.OriginalName, $installed.ProviderName
    }
    if ($null -ne $installed -and $installed.ProviderName) {
        return '{0} ({1})' -f $Device.DriverName, $installed.ProviderName
    }
    return $Device.DriverName
}

function Get-Verdict ($Device, [bool] $Live) {
    $infName = [IO.Path]::GetFileName($InfPath)
    $matched = $Device.CompatibleIds -contains $InfMatchId
    $installed = @($Device.Drivers | Where-Object { $_.DriverName -eq $Device.DriverName }) | Select-Object -First 1
    $originalName = ''
    if ($null -ne $installed) {
        $originalName = $installed.OriginalName
    }
    # Driver names compare without case, as libusb compares them
    $viaLibusbK = $LibusbKDrivers -contains $Device.Service
    $lines = @()
    if ($Device.Service -eq 'WinUSB' -or $viaLibusbK) {
        $driver = 'WinUSB'
        if ($viaLibusbK) {
            $driver = $Device.Service
        }
        $guid = $null
        $dll = $null
        if ($Live) {
            $guid = Test-InterfaceGuid $Device.InstanceId
            if ($viaLibusbK) {
                $dll = Test-LibusbKDll
            }
        }
        if ($dll -eq $false) {
            $lines += ('cannot open it: libusb drives {0} through libusbK.dll, which is missing from the System32' -f
                       $driver)
            $lines += 'folder of Windows. Install libusbK.dll there, or bind WinUSB instead: see the documentation.'
        } elseif ($guid -eq $false) {
            $lines += ('cannot open it yet: {0} is its driver, but the DeviceInterfaceGUIDs registry value' -f $driver)
            $lines += 'that libusb needs is missing. See "Device Manager" in the documentation.'
        } elseif ($Device.DriverName -eq 'winusb.inf' -and $null -eq $guid) {
            $lines += 'can open it once its DeviceInterfaceGUIDs registry value is set, which Device Manager'
            $lines += 'does not do. See "Device Manager" in the documentation.'
        } elseif ($viaLibusbK) {
            if ($dll) {
                $lines += ('can open it: libusb drives {0} through libusbK.dll, which is installed.' -f $driver)
            } else {
                $lines += ('can open it if libusbK.dll, through which libusb drives {0}, is in the System32' -f $driver)
                $lines += 'folder of Windows.'
            }
            $lines += 'WinUSB, which needs no other DLL, is the recommended driver: see the documentation.'
        } elseif ($originalName -eq $infName -or $Device.DriverName -eq 'winusb.inf') {
            $lines += 'can open it.'
        } else {
            $lines += 'can open it while no other program, such as the software of the camera vendor, has it open.'
        }
    } elseif ($Device.Service) {
        $lines += ('cannot open it: its driver is {0}, which libusb does not support.' -f $Device.Service)
        $lines += ('{0} never replaces another driver: see the documentation to switch drivers.' -f $infName)
    } elseif ($matched) {
        $lines += ('cannot open it: it has no driver. Install {0} with this script''s install action,' -f $infName)
        $lines += 'or see the documentation.'
    } else {
        $lines += 'cannot open it: it has no driver.'
    }
    if (-not $matched) {
        $lines += ('{0} does not match it: its compatible IDs lack {1}.' -f $infName, $InfMatchId)
        $lines += 'Bind WinUSB with Zadig or Device Manager instead, see the documentation.'
    }
    $staged = @($Device.Drivers |
                Where-Object { $_.OriginalName -eq $infName -and $_.DriverName -ne $Device.DriverName })
    foreach ($driver in $staged) {
        $lines += ('{0} ({1}) matches it too, but ranks below its driver.' -f $infName, $driver.DriverName)
    }
    return $lines
}

function Show-Cameras {
    $result = Get-UsbVisionDevices
    $live = -not $DeviceListFile
    Write-Output ('USB3 Vision cameras, from {0}:' -f $result.Source)
    if ($result.Devices.Count -eq 0) {
        Write-Output ''
        Write-Output ('No USB3 Vision camera found: no connected device has the compatible ID {0}.' -f $UsbVisionId)
        Write-Output 'A camera bound to WinUSB as a whole, at its composite device node, is not listed.'
    }
    foreach ($device in $result.Devices) {
        Write-Output ''
        Write-Output $device.InstanceId
        Write-Output ('    Name:   {0}' -f $device.Description)
        if ($device.InstanceId -match 'VID_([0-9A-F]{4})&PID_([0-9A-F]{4})(&MI_([0-9A-F]{2}))?') {
            $ids = 'vendor ID 0x{0}, product ID 0x{1}' -f $Matches[1], $Matches[2]
            if ($Matches[4]) {
                $ids += ', interface {0}' -f [Convert]::ToInt32($Matches[4], 16)
            }
            Write-Output ('    IDs:    {0}' -f $ids)
        }
        Write-Output ('    Status: {0}' -f ((@($device.Status) + $device.Problems) -join ', '))
        if ($device.Service) {
            Write-Output ('    Driver: {0}, from {1}' -f $device.Service, (Format-Package $device))
        } else {
            Write-Output '    Driver: none'
        }
        $verdict = @(Get-Verdict $device $live)
        Write-Output ('    Aravis: {0}' -f $verdict[0])
        foreach ($line in @($verdict | Select-Object -Skip 1)) {
            Write-Output ('            {0}' -f $line)
        }
    }
    Write-Output ''
    Write-Output ('Documentation: {0}' -f $DocumentationUrl)
}

function Get-CatalogPath ([string] $Inf) {
    # The CatalogFile entry of the [Version] section
    $section = ''
    foreach ($line in [IO.File]::ReadAllLines($Inf)) {
        $line = ($line -replace ';.*$', '').Trim()
        if ($line -match '^\[(.+)\]$') {
            $section = $Matches[1].Trim()
        } elseif ($section -eq 'Version' -and $line -match '^CatalogFile\s*=\s*(.+)$') {
            return Join-Path (Split-Path -Parent $Inf) $Matches[1].Trim()
        }
    }
    throw "$Inf has no CatalogFile entry in its [Version] section."
}

function Test-Catalog ([string] $Catalog) {
    # Returns the reasons why pnputil would refuse the driver package
    if (-not (Test-Path -LiteralPath $Catalog)) {
        return , @(('{0} does not exist. Windows installs only signed driver packages: ' -f $Catalog) +
                   'create the catalog with MakeCat and sign it with SignTool, see Get-Help on this script ' +
                   'or the documentation.')
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Catalog
    if ($signature.Status -eq 'NotSigned' -or $null -eq $signature.SignerCertificate) {
        return , @(('{0} is not signed: sign it with SignTool, ' -f $Catalog) +
                   'see Get-Help on this script or the documentation.')
    }
    $reasons = @()
    if ($signature.Status -ne 'Valid') {
        $reasons += ('The signature of {0} is not valid on this machine ({1}): {2} Add the signing certificate ' -f
                     $Catalog, $signature.Status, $signature.StatusMessage) +
                    'to the Trusted Root Certification Authorities store of the local machine.'
    }
    $thumbprint = $signature.SignerCertificate.Thumbprint
    if (-not (Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPublisher\' + $thumbprint))) {
        $reasons += ('The certificate that signed {0} ({1}) is not in the Trusted Publishers store of the ' -f
                     $Catalog, $signature.SignerCertificate.Subject) +
                    'local machine, so Windows would not install the driver package without asking.'
    }
    return , $reasons
}

function Assert-Administrator ([string] $What) {
    if (-not $WhatIfPreference -and -not (Test-Administrator)) {
        throw ("$What needs administrator rights: run it from an elevated PowerShell, " +
               'or add -WhatIf to see the commands.')
    }
}

function Install-Package {
    if (-not (Test-Path -LiteralPath $InfPath)) {
        throw "$InfPath does not exist."
    }
    $inf = (Resolve-Path -LiteralPath $InfPath).ProviderPath
    foreach ($reason in (Test-Catalog (Get-CatalogPath $inf))) {
        if ($WhatIfPreference) {
            Write-Warning $reason
        } else {
            throw $reason
        }
    }
    Assert-Administrator 'install'
    $arguments = @('/add-driver', $inf, '/install')
    $command = Format-Command $arguments
    if ($PSCmdlet.ShouldProcess($command, "Run ${command}?", 'Install the Aravis WinUSB driver package')) {
        & (Get-PnputilPath) @arguments | Out-Host
        switch ($LASTEXITCODE) {
            0 { }
            3010 { Write-Warning 'Restart Windows to finish the installation.' }
            259 {
                Write-Output (('{0} is in the driver store, but no camera was updated: none is connected, ' -f $inf) +
                              'or each one has a better-ranked driver, such as its vendor''s.')
            }
            default { throw "$command failed with exit code $LASTEXITCODE. See $env:SystemRoot\INF\setupapi.dev.log." }
        }
        Write-Output ''
        Show-Cameras
    }
}

function ConvertFrom-DriverXml ([string] $Text) {
    $document = New-Object System.Xml.XmlDocument
    $document.LoadXml((Repair-PnputilXml $Text))
    $packages = foreach ($node in $document.SelectNodes('/PnpUtil/Driver')) {
        [pscustomobject] @{
            PublishedName = $node.GetAttribute('DriverName')
            OriginalName = Get-ChildText $node 'OriginalName'
        }
    }
    return , @($packages)
}

function ConvertFrom-DriverText ([string[]] $Lines) {
    $packages = foreach ($record in (ConvertFrom-PnputilText $Lines 'Published Name')) {
        [pscustomobject] @{
            PublishedName = Get-TextValue $record 'Published Name'
            OriginalName = Get-TextValue $record 'Original Name'
        }
    }
    return , @($packages)
}

function Get-DriverPackages {
    if ($DriverListFile) {
        $text = Read-ListFile $DriverListFile
        $source = $DriverListFile
        if (Test-Xml $text) {
            return ConvertFrom-DriverXml $text
        }
        $lines = $text -split '\r?\n'
    } else {
        $source = 'the output of ' + (Format-Command @('/enum-drivers'))
        $text = Get-PnputilXml @('/enum-drivers')
        if ($null -ne $text) {
            try {
                return ConvertFrom-DriverXml $text
            } catch {
                Write-Warning ('Could not read {0} /format xml: {1}' -f $source, $_.Exception.Message)
            }
        }
        $lines = Get-PnputilText @('/enum-drivers')
    }
    $packages = ConvertFrom-DriverText $lines
    if (Test-UnreadText $packages $lines) {
        # Saying that the package is not in the driver store could be wrong
        throw (('Could not read {0}, which is probably not in English. Run pnputil /enum-drivers, ' -f $source) +
               ('find the published name, oemNN.inf, of each package whose original name is {0}, ' -f
                [IO.Path]::GetFileName($InfPath)) +
               'and remove it with pnputil /delete-driver oemNN.inf /uninstall.')
    }
    return , $packages
}

function Uninstall-Package {
    if ($DriverListFile -and -not $WhatIfPreference) {
        throw '-DriverListFile only works with -WhatIf: a saved driver list may not describe this machine.'
    }
    $infName = [IO.Path]::GetFileName($InfPath)
    $all = Get-DriverPackages
    $packages = @($all | Where-Object { $_.OriginalName -eq $infName })
    if ($packages.Count -eq 0) {
        Write-Output "$infName is not in the driver store."
        return
    }
    Assert-Administrator 'uninstall'
    foreach ($package in $packages) {
        $arguments = @('/delete-driver', $package.PublishedName, '/uninstall')
        $command = Format-Command $arguments
        if ($PSCmdlet.ShouldProcess($command, "Run ${command}?", 'Remove the Aravis WinUSB driver package')) {
            & (Get-PnputilPath) @arguments | Out-Host
            if ($LASTEXITCODE -eq 3010) {
                Write-Warning 'Restart Windows to finish the removal.'
            } elseif ($LASTEXITCODE -ne 0) {
                throw "$command failed with exit code $LASTEXITCODE. See $env:SystemRoot\INF\setupapi.dev.log."
            }
        }
    }
    Write-Output 'The signing certificate stays in the certificate stores: remove it there if nothing else uses it.'
}

try {
    if (-not $InfPath) {
        $InfPath = Join-Path $PSScriptRoot 'aravis-u3v.inf'
    }
    switch ($Action) {
        'list' { Show-Cameras }
        'install' { Install-Package }
        'uninstall' { Uninstall-Package }
    }
    exit 0
} catch {
    [Console]::Error.WriteLine('arv-winusb-setup: ' + $_.Exception.Message)
    exit 1
}
