============================================
Ceph-Dokan
============================================

CephFS Client on Win32 based on Dokan 0.6.0

How to use
------------

First install Dokan 0.6.0 on your Windows.

Dokan 0.6.0 now support Windows XP/Vista/7/2008. 

After compile from source it can support Win8.

If you compile dokan yourself, you will need to sign the dokan.sys with your own cert.

Now you can use ceph-dokan on your Windows and get full speed access to CephFS without slowly Samba.

    ceph-dokan.exe
     -c CephConf  (ex. /r c:\ceph.conf)
     -l DriveLetter (ex. /l m)

Example:  ceph-dokan.exe -c ceph.conf -l m

Then you will see a new Drive[M:] in your explorer.


Introduction
-----------

This program is based on original Ceph code and modifid for Win32 Platform.

It is compiled by gcc, so it is native Win32 program with only one extra dll "pthreadGC2.dll".

Future
-----------
Ceph-Dokan will get continuous improvement and upgrade with upstream Ceph code.

