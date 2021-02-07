# zfsync
MultiThreaded ZFS Dataset Synchro Tool (working on FreeBSD 12.2)

# About the author
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover<br>
Twitter : @cedricmaunoury<br>
Linkedin : cedric-maunoury<br>
**I'm currently looking for a remote job (from the Netherlands)**

# How is this working ?!?
This project is still very young, so many updates will come to improve the code and make it clean.
ZFSync is made of two binaries : zfsync_send & zfsync_recv<br>
zfsync_recv is in charge of receiving updates from zfsync_send (launched by the cron job zfsyncron.sh)<br>
```
+----------------+               +----------------+
|                | ----ds1--->   |                |
|     SENDER     | ----ds2--->   |    RECEIVER    |
|                | ----ds3--->   |                |
+----------------+               +----------------+
 zpool/test_local                zpool/test_remote
 ```

1. zfsyncron.sh creates snapshot on the Dataset to be synced and its children (and destroy the oldest ones, according to retention settings)
2. It launches zfsync_send to send Dataset child snaps to zfsync_recv in a multithreaded way
3. Each thread is doing the following job :
    - It sends the snapshots name available for the current zhp to the zfsync_recv
      - -> *snap0@snap1@snap2:CHILDRENNAME:sync*
    - The receiver (zfsync_recv) replies with the last snapshot in common (or NEW or FULL if nothing matches)
      - <- *0:(snapX|NEW|FULL)*
    - Then a zfs_send is launched using the receiver reply to set the "from" in zfs_send
      - -> *0000110000111101011000... :)*
    - If there's a new child dataset to handle, it uses the same connection to do the same work
    - ...
    - ...
    - When there is nothing more to do, each thread in zfsync_send but the last one sends the following message to close the connection
      - -> *:close*
    - The last thread sends the following message to ask the zfsync_recv binary to clean its buffer (see below for more infos)
      - -> *:cleanbuffer*

# What is the "buffer" in zfsync_recv ?
As we can't be sure about the order the ZFS streams will come, in some situations, the parent dataset does not exist on the RECEIVER. In this case, the ZFS stream will not be received directly in its target place, but in a "buffer" dataset with an hexadecimal name.

# How to use it :
- on the SENDER host, the dataset to be synced is $DatasetS => Put a cron to launch zfsyncron.sh regularly
- on the RECEVIER host, the target dataset ($DatasetR) can have another name. $DatasetR will be updated => Launch ZFSync_Recv (to avoid issues, consider setting readonly=on on "remote" dataset)

# What is not handled for the moment by this code : 
- If a dataset is destroyed on the source, it is not on the remote side
- No SSL... everything is clear on the network (could be very interesting as only one connection is opened by thread)

# Possible improvments :
- if we can detect that there's no change on the datas between two child snapshots, it could be interesting not to send the diff

# Compilation
 ```
:~/gitrepo/zfsync $ ./configure
:~/gitrepo/zfsync $ make
:~/gitrepo/zfsync $ make install
 ```
# Crontab example on the SENDER
 ```
* * * * * /usr/local/bin/zfsyncron.sh -p 30 -o /var/log/zfsync_send.log zsys/home/zfsync_send/local 127.0.0.1 >> /var/log/zfsyncron.log 2>&1
 ```
 
# Command example on the RECEIVER
 ```
/usr/local/bin/zfsync_recv -v -p 30 -o /var/log/zfsync_recv.log zsys/home/zfsync_recv/remote
 ```
 
# Full example to test on one host
```
# zfs create -p zsys/home/zfsync_send/local/test1/test1.1/test1.1.1
# zfs create -p zsys/home/zfsync_recv
# zfs list | grep zfsync
zsys/home/zfsync_recv                                  96K  2.10G    96K  /zsys/home/zfsync_recv
zsys/home/zfsync_send                                 480K  2.10G    96K  /zsys/home/zfsync_send
zsys/home/zfsync_send/local                           384K  2.10G    96K  /zsys/home/zfsync_send/local
zsys/home/zfsync_send/local/test1                     288K  2.10G    96K  /zsys/home/zfsync_send/local/test1
zsys/home/zfsync_send/local/test1/test1.1             192K  2.10G    96K  /zsys/home/zfsync_send/local/test1/test1.1
zsys/home/zfsync_send/local/test1/test1.1/test1.1.1    96K  2.10G    96K  /zsys/home/zfsync_send/local/test1/test1.1/test1.1.1
# crontab -l
* * * * * /usr/local/bin/zfsyncron.sh -p 30 -o /var/log/zfsync_send.log zsys/home/zfsync_send/local 127.0.0.1 >> /var/log/zfsyncron.log 2>&1
# /usr/local/bin/zfsync_recv -v -p 30 -o /var/log/zfsync_recv.log zsys/home/zfsync_recv/remote

```
Open a new shell
```
# tail -f /var/log/zfsync_recv.log
27/01/2021 06:36:00:120849 | th:11120640 | fd:25 | New connection
27/01/2021 06:36:00:120949 | th:11121920 | fd:25 | connbuf='@20210127-0636_M::sync' (size:256)
27/01/2021 06:36:00:128411 | th:11120640 | fd:26 | New connection
27/01/2021 06:36:00:128585 | th:11123200 | fd:26 | connbuf='@20210127-0636_M:/test1:sync' (size:256)
27/01/2021 06:36:00:187163 | th:11121920 | fd:25 | (:sync) Sending on fd : 0:OK
27/01/2021 06:36:00:187980 | th:11121920 | fd:25 | connbuf='@20210127-0636_M:/test1/test1.1:sync' (size:256)
27/01/2021 06:36:00:245064 | th:11123200 | fd:26 | (/test1:sync) Sending on fd : 0:OK
27/01/2021 06:36:00:245191 | th:11123200 | fd:26 | connbuf='@20210127-0636_M:/test1/test1.1/test1.1.1:sync' (size:256)
27/01/2021 06:36:00:246852 | th:11121920 | fd:25 | (/test1/test1.1:sync) Sending on fd : 0:OK
27/01/2021 06:36:00:250005 | th:11121920 | fd:25 | connbuf=':close' (size:256)
27/01/2021 06:36:00:302743 | th:11123200 | fd:26 | (/test1/test1.1/test1.1.1:sync) Sending on fd : 0:OK
27/01/2021 06:36:00:302884 | th:11123200 | fd:26 | connbuf=':cleanbuffer' (size:256)
27/01/2021 06:36:00:304345 | th:11123200 | Buffered dataset found : 2F74657374312F74657374312E31
27/01/2021 06:36:00:304366 | th:11123200 | hex2bin : zsys/home/zfsync_recv/remote/test1/test1.1
27/01/2021 06:36:00:304456 | th:11123200 | Parent dataset does not exist (zsys/home/zfsync_recv/remote/test1). We have to rename buffered parent dataset first
27/01/2021 06:36:00:304473 | th:11123200 | Buffered parent dataset : 2F7465737431
27/01/2021 06:36:00:305170 | th:11123200 | Buffered dataset found : 2F7465737431
27/01/2021 06:36:00:305189 | th:11123200 | hex2bin : zsys/home/zfsync_recv/remote/test1
27/01/2021 06:36:00:305694 | th:11123200 | Renaming 2F7465737431 to zsys/home/zfsync_recv/remote/test1
27/01/2021 06:36:00:318463 | th:11123200 | Rename OK
27/01/2021 06:36:00:318512 | th:11123200 | Buffered parent dataset has been succesfully renamed
27/01/2021 06:36:00:318526 | th:11123200 | Renaming 2F74657374312F74657374312E31 to zsys/home/zfsync_recv/remote/test1/test1.1
27/01/2021 06:36:00:331378 | th:11123200 | Rename OK
27/01/2021 06:36:00:333282 | th:11123200 | Buffered dataset found : 2F74657374312F74657374312E312F74657374312E312E31
27/01/2021 06:36:00:333303 | th:11123200 | hex2bin : zsys/home/zfsync_recv/remote/test1/test1.1/test1.1.1
27/01/2021 06:36:00:334012 | th:11123200 | Renaming 2F74657374312F74657374312E312F74657374312E312E31 to zsys/home/zfsync_recv/remote/test1/test1.1/test1.1.1
27/01/2021 06:36:00:348718 | th:11123200 | Rename OK
27/01/2021 06:36:00:349218 | th:11123200 | fd:26 | (:cleanbuffer) Sending on fd : 0:OK
^C
# zfs list | grep zfsync
zsys/home/zfsync_recv                                  576K  2.10G    96K  /zsys/home/zfsync_recv
zsys/home/zfsync_recv/remote                           384K  2.10G    96K  /zsys/home/zfsync_recv/remote
zsys/home/zfsync_recv/remote.zfsyncbuffer               96K  2.10G    96K  /zsys/home/zfsync_recv/remote.zfsyncbuffer
zsys/home/zfsync_recv/remote/test1                     288K  2.10G    96K  /zsys/home/zfsync_recv/remote/test1
zsys/home/zfsync_recv/remote/test1/test1.1             192K  2.10G    96K  /zsys/home/zfsync_recv/remote/test1/test1.1
zsys/home/zfsync_recv/remote/test1/test1.1/test1.1.1    96K  2.10G    96K  /zsys/home/zfsync_recv/remote/test1/test1.1/test1.1.1
zsys/home/zfsync_send                                  480K  2.10G    96K  /zsys/home/zfsync_send
zsys/home/zfsync_send/local                            384K  2.10G    96K  /zsys/home/zfsync_send/local
zsys/home/zfsync_send/local/test1                      288K  2.10G    96K  /zsys/home/zfsync_send/local/test1
zsys/home/zfsync_send/local/test1/test1.1              192K  2.10G    96K  /zsys/home/zfsync_send/local/test1/test1.1
zsys/home/zfsync_send/local/test1/test1.1/test1.1.1     96K  2.10G    96K  /zsys/home/zfsync_send/local/test1/test1.1/test1.1.1
# 
```
