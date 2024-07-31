# Linux driver for STMicroelectronics Unicorn II chipset
### Info
This repository contains the driver for once popular ADSL modem chipset embedded in several low-cost USB (and PCI) CPE devices e.g. ZTE ZXDSL 852 (version 2), Siemens A-100 or BeWAN ADSL modem.

### Compatibility
Originally this driver was written with Linux 2.2-2.4 and GCC 3 in mind (especially Fedora and Mandrake distributions). Then it was ported to Linux 2.6 and 3.0 by Mariusz Smykuła. A last touch on the driver had Zbigniew Łuszpiński porting it onto Linux 3.6.10. Currently it is the newest fully supported kernel version.

### What is with this repo then?
This repository contains also the driver but ported to compile on kernel version 4, 5 and even 6. Word "compile" is here not without reason. Although the module compiles successfully, it does not run. The issue lies within the file `modem_ant_USB_LINUX.o.regparm3` - a closed source blob provided straight from STMicroelectronics which was compiled using GCC 3.0, as such newer versions of the compiler won't properly link it into the module and kernel fails loading it.

The fact that this blob is a 32-bit library, also does not help. Even if the kernel would load it properly, the module will work only on 32-bit OSes.

### How to compile?
Just install your standard development tools (`build-essential` on debian-based distros) and your kernel's headers. Then invoke command `make -f <file>.mk`, where `<file>` is either `KernelOld.mk` or `KernelNew.mk`.

`KernelOld.mk` is for compilation on Linux 2.2-2.6 and 3.6.10, `KernelNew.mk` is for newer kernels. I would recommend to try first the old one and if it fails - the new one.