# The WinUSB driver package for USB3 Vision cameras on Windows, the MakeCat
# definition of its catalog, and a PowerShell helper that lists the cameras and
# installs or removes the package. tests/meson.build checks them on every
# platform.

winusb_inf = files ('aravis-u3v.inf')
winusb_cdf = files ('aravis-u3v.cdf')
winusb_setup_script = files ('arv-winusb-setup.ps1')

if host_machine.system () == 'windows' and usb_dep.found ()
  install_data (winusb_inf, winusb_cdf, winusb_setup_script, install_dir: aravis_data_dir / 'winusb')
endif
