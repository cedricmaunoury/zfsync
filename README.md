# zfsync
MultiThreaded ZFS Dataset Synchro Tool

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

