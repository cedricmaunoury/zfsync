# zfsync
MultiThreaded ZFS Dataset Synchro Tool (working on FreeBSD 12.2)

# About the author
This code has been written by Cedric MAUNOURY, a french FreeBSD & ZFS lover<br>
Twitter : @cedricmaunoury<br>
Linkedin : cedric-maunoury<br>
**I'm currently looking for a remote job (from the Netherlands)**

# How is this working ?!?
ZFSync is made of two binaries : zfsync_send & zfsync_recv<br>
zfsync_recv is in charge of receiving updates from zfsync_send (launched by the cron job zfsyncron.sh)<br>
```
+----------------+               +----------------+
|                | ----ds1--->   |                |
|     SENDER     | ----ds2--->   |    RECEIVER    |
|                | ----ds3--->   |                |
+----------------+               +----------------+
 zpool/test/local                zpool/test/remote
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
    - When there is nothing more to do, it sends the following message to close the connection
      - -> *END:sync*

# How to use it :
- on the SENDER host, the dataset to be synced is $DatasetS. The children of $Dataset/local will be synced => Put a cron to launch zfsyncron.sh regularly
- on the RECEVIER host, the target dataset ($DatasetR) can have another name. The children of $Dataset/remote will be updated => Launch ZFSync_Recv (to avoid issues, consider setting readonly=on on "remote" dataset)
- Why local and remote subdataset ? The goal is to ensure no data loss in case of a "Split Brain" in a cluster architecture.

# What is not handled for the moment by this code : 
- If a child dataset is destroyed on the source, it is not on the remote side
- No SSL... everything is clear on the network (could be very interesting as only one connection is opened by thread)
- Only a depth of 1 is handled. If you create a child dataset in a child dataset, it won't break anything, but it won't be synced.

# Possible improvments :
- if we can detect that there's no change on the datas between two child snapshots, it could be interesting not to send the diff
- the zfsync_recv program does not create directories to store its logs. You have to create /var/log/zfsync to store the thread log files

# Compilation
 ```
:~/gitrepo/zfsync $ export SRCTOP=/usr/src
:~/gitrepo/zfsync $ cd send
:~/gitrepo/zfsync/send $ make
Warning: Object directory not changed from original /zsys/home/gc/gitrepo/zfsync/send
:~/gitrepo/zfsync/send $ cd ../recv/
:~/gitrepo/zfsync/recv $ make
Warning: Object directory not changed from original /zsys/home/gc/gitrepo/zfsync/recv
:~/gitrepo/zfsync/recv $ 
 ```
# Crontab example on the SENDER
 ```
* * * * * $YOURDIR/zfsyncron.sh -p 30 -t 2 zroot/home/zfshads 127.0.0.1 >> /var/log/zfsyncron.log 2>&1
 ```
 
# Command example on the RECEIVER
 ```
mkdir /var/log/zfsync_recv
$YOURDIR/zfsync_recv -p 30 -o /var/log/zfsync_recv.log zsys/home/zfshads
 ```
 
# Full example to test on one host
```
root@freebsd_1:/home/gitrepo/zfsync/recv # zfs create zsys/home/zfsync_send
root@freebsd_1:/home/gitrepo/zfsync/recv # zfs create zsys/home/zfsync_send/local
root@freebsd_1:/home/gitrepo/zfsync/recv # zfs create zsys/home/zfsync_recv
root@freebsd_1:/home/gitrepo/zfsync/recv # zfs create -o readonly=on zsys/home/zfsync_recv/remote
root@freebsd_1:/home/gitrepo/zfsync/recv # mkdir /home/log
root@freebsd_1:/home/gitrepo/zfsync/recv # crontab -l
* * * * * /home/gitrepo/zfsync/zfsyncron.sh -p 30 zsys/home/zfsync_send 127.0.0.1 >> /home/log/zfsync_send.log 2>&1
root@freebsd_1:/home/gitrepo/zfsync/recv # mkdir /home/log/zfsync_recv
root@freebsd_1:/home/gitrepo/zfsync/recv # ./zfsync_recv -p 30 -o  /home/log/zfsync_recv.log zsys/home/zfsync_recv
```
Open a new shell
```
root@freebsd_1:~ # zfs create zsys/home/zfsync_send/local/test1
root@freebsd_1:~ # sleep 60
root@freebsd_1:~ # zfs list -t snapshot | egrep "zfsync_.*/test1"
zsys/home/zfsync_recv/remote/test1@20210122-1531_M      0      -    96K  -
zsys/home/zfsync_send/local/test1@20210122-1531_M       0      -    96K  -
root@freebsd_1:~ # 
root@freebsd_1:~ # timeout 1 yes > /home/zfsync_send/local/test1/yes.log
root@freebsd_1:~ # zfs list -t snapshot | egrep "zfsync_.*/test1"
zsys/home/zfsync_recv/remote/test1@20210122-1531_M      0      -    96K  -
zsys/home/zfsync_recv/remote/test1@20210122-1532_M      0      -    96K  -
zsys/home/zfsync_recv/remote/test1@20210122-1533_M      0      -    96K  -
zsys/home/zfsync_recv/remote/test1@20210122-1534_M      0      -  15.7M  -
zsys/home/zfsync_send/local/test1@20210122-1531_M       0      -    96K  -
zsys/home/zfsync_send/local/test1@20210122-1532_M       0      -    96K  -
zsys/home/zfsync_send/local/test1@20210122-1533_M       0      -    96K  -
zsys/home/zfsync_send/local/test1@20210122-1534_M       0      -  15.7M  -
root@freebsd_1:~ # 
```
Logs make it easy to understand it is multi threaded (/test1 & /test2 streams are sent in parallel)
```
root@freebsd_1:/zsys/home/log # tail -10 /home/log/zfsync_recv.log
22/01/2021 15:50:00:161356 | th:11120384 | fd:22 | (/test1:sync) Sending on fd : 0:OK
22/01/2021 15:50:00:161439 | th:11120384 | fd:22 | connbuf='END:sync' (size:256)
22/01/2021 15:51:00:074002 | th:11116544 | fd:24 | New connection
22/01/2021 15:51:00:074226 | th:11121664 | fd:24 | connbuf='@20210122-1549_M@20210122-1550_M@20210122-1551_M:/test2:sync' (size:256)
22/01/2021 15:51:00:075601 | th:11116544 | fd:22 | New connection
22/01/2021 15:51:00:075814 | th:11117824 | fd:22 | connbuf='@20210122-1548_M@20210122-1549_M@20210122-1550_M@20210122-1551_M:/test1:sync' (size:256)
22/01/2021 15:51:00:155805 | th:11121664 | fd:24 | (/test2:sync) Sending on fd : 0:OK
22/01/2021 15:51:00:160097 | th:11117824 | fd:22 | (/test1:sync) Sending on fd : 0:OK
22/01/2021 15:51:00:160194 | th:11117824 | fd:22 | connbuf='END:sync' (size:256)
22/01/2021 15:51:00:160295 | th:11121664 | fd:24 | connbuf='END:sync' (size:256)
root@freebsd_1:/zsys/home/log # 
 ```
