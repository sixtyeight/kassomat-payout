# Libraries

 - [Hiredis][hiredis]
 - [libevent][libevent]
 - [libuuid][libuuid]
 - [SSP protocol example code from Innovative Technology][itl]

# Hardware Documentation

 - [Münzprüfer RM5 CC-Talk Dokumentation][rm5]
 - [Innovative Technology Smart Hopper][itl-sh]
 - [Innovative Technology NV200][itl-nv200]

# udev rule
``SUBSYSTEM=="tty" ATTRS{manufacturer}=="Innovative Technology LTD" SYMLINK+="kassomat"``

[hiredis]: https://github.com/redis/hiredis
[libevent]: http://libevent.org
[libuuid]: http://sourceforge.net/projects/libuuid
[itl]: http://innovative-technology.com
[itl-sh]: http://innovative-technology.com/images/pdocuments/manuals/SMART_Hopper_manual_set.zip
[itl-nv200]: http://innovative-technology.com/images/pdocuments/manuals/NV200%20manual%20set.zip
[rm5]: http://www.aus.at/download/RM5german.pdf
